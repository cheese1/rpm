#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>

#include "spec.h"
#include "header.h"
#include "misc.h"
#include "reqprov.h"
#include "names.h"
#include "macro.h"

#include "rpmlib.h"
#include "files.h"
#include "lib/cpio.h"
#include "lib/misc.h"
#include "lib/signature.h"
#include "lib/rpmlead.h"
#include "lib/messages.h"

#define RPM_MAJOR_NUMBER 3

static int processScriptFiles(Spec spec, Package pkg);
static int addFileToTag(Spec spec, char *file, Header h, int tag);
static int writeRPM(Header header, char *fileName, int type,
		    struct cpioFileMapping *cpioList, int cpioCount,
		    char *passPhrase, char **cookie);
static int cpio_gzip(int fd, struct cpioFileMapping *cpioList,
		     int cpioCount, int *archiveSize);

int packageSources(Spec spec)
{
    char *name, *version, *release;
    char fileName[BUFSIZ];
    HeaderIterator iter;
    int_32 tag, count;
    char **ptr;

    /* Add some cruft */
    headerAddEntry(spec->sourceHeader, RPMTAG_RPMVERSION,
		   RPM_STRING_TYPE, VERSION, 1);
    headerAddEntry(spec->sourceHeader, RPMTAG_BUILDHOST,
		   RPM_STRING_TYPE, buildHost(), 1);
    headerAddEntry(spec->sourceHeader, RPMTAG_BUILDTIME,
		   RPM_INT32_TYPE, getBuildTime(), 1);

    headerGetEntry(spec->sourceHeader, RPMTAG_NAME,
		   NULL, (void **)&name, NULL);
    headerGetEntry(spec->sourceHeader, RPMTAG_VERSION,
		   NULL, (void **)&version, NULL);
    headerGetEntry(spec->sourceHeader, RPMTAG_RELEASE,
		   NULL, (void **)&release, NULL);
    sprintf(fileName, "%s-%s-%s.%ssrc.rpm", name, version, release,
	    spec->noSource ? "no" : "");
    spec->sourceRpmName = strdup(fileName);
    sprintf(fileName, "%s/%s", rpmGetVar(RPMVAR_SRPMDIR), spec->sourceRpmName);

    /* Add the build restrictions */
    iter = headerInitIterator(spec->buildRestrictions);
    while (headerNextIterator(iter, &tag, NULL, (void **)&ptr, &count)) {
	headerAddEntry(spec->sourceHeader, tag,
		       RPM_STRING_ARRAY_TYPE, ptr, count);
	FREE(ptr);
    }
    headerFreeIterator(iter);
    if (spec->buildArchitectureCount) {
	headerAddEntry(spec->sourceHeader, RPMTAG_BUILDARCHS,
		       RPM_STRING_ARRAY_TYPE,
		       spec->buildArchitectures, spec->buildArchitectureCount);
    }

    FREE(spec->cookie);
    
    return writeRPM(spec->sourceHeader, fileName, RPMLEAD_SOURCE,
		    spec->sourceCpioList, spec->sourceCpioCount,
		    spec->passPhrase, &(spec->cookie));
}

int packageBinaries(Spec spec)
{
    int rc;
    char *binFormat, *binRpm, *errorString;
    char *name, fileName[BUFSIZ];
    Package pkg;

    pkg = spec->packages;
    while (pkg) {
	if (!pkg->fileList) {
	    pkg = pkg->next;
	    continue;
	}

	if ((rc = processScriptFiles(spec, pkg))) {
	    return rc;
	}
	
	generateAutoReqProv(spec, pkg, pkg->cpioList, pkg->cpioCount);
	printReqs(spec, pkg);
	
	if (spec->cookie) {
	    headerAddEntry(pkg->header, RPMTAG_COOKIE,
			   RPM_STRING_TYPE, spec->cookie, 1);
	}
	
	headerAddEntry(pkg->header, RPMTAG_RPMVERSION,
		       RPM_STRING_TYPE, VERSION, 1);
	headerAddEntry(pkg->header, RPMTAG_BUILDHOST,
		       RPM_STRING_TYPE, buildHost(), 1);
	headerAddEntry(pkg->header, RPMTAG_BUILDTIME,
		       RPM_INT32_TYPE, getBuildTime(), 1);
	headerAddEntry(pkg->header, RPMTAG_SOURCERPM, RPM_STRING_TYPE,
		       spec->sourceRpmName ?
		       spec->sourceRpmName : "(unknown)", 1);
	
	binFormat = rpmGetVar(RPMVAR_RPMFILENAME);
	binRpm = headerSprintf(pkg->header, binFormat, rpmTagTable,
			       rpmHeaderFormats, &errorString);
	if (!binRpm) {
	    headerGetEntry(pkg->header, RPMTAG_NAME, NULL,
			   (void **)&name, NULL);
	    rpmError(RPMERR_BADFILENAME, "Could not generate output "
		     "filename for package %s: %s\n", name, errorString);
	    return RPMERR_BADFILENAME;
	}
	sprintf(fileName, "%s/%s", rpmGetVar(RPMVAR_RPMDIR), binRpm);
	FREE(binRpm);

	if ((rc = writeRPM(pkg->header, fileName, RPMLEAD_BINARY,
			   pkg->cpioList, pkg->cpioCount,
			   spec->passPhrase, NULL))) {
	    return rc;
	}
	
	pkg = pkg->next;
    }
    
    return 0;
}

static int writeRPM(Header header, char *fileName, int type,
		    struct cpioFileMapping *cpioList, int cpioCount,
		    char *passPhrase, char **cookie)
{
    int archiveSize, fd, ifd, rc, count, arch, os, sigtype;
    char *sigtarget, *name, *version, *release;
    char buf[BUFSIZ];
    Header sig;
    struct rpmlead lead;

    /* Add the a bogus archive size to the Header */
    headerAddEntry(header, RPMTAG_ARCHIVESIZE, RPM_INT32_TYPE,
		   &archiveSize, 1);

    /* Create and add the cookie */
    if (cookie) {
	sprintf(buf, "%s %d", buildHost(), (int) time(NULL));
	*cookie = strdup(buf);
	headerAddEntry(header, RPMTAG_COOKIE, RPM_STRING_TYPE, *cookie, 1);
    }
    
    /* Write the header */
    if (makeTempFile(NULL, &sigtarget, &fd)) {
	rpmError(RPMERR_CREATE, "Unable to open temp file");
	return RPMERR_CREATE;
    }
    headerWrite(fd, header, HEADER_MAGIC_YES);
	     
    /* Write the archive and get the size */
    if ((rc = cpio_gzip(fd, cpioList, cpioCount, &archiveSize))) {
	close(fd);
	unlink(sigtarget);
	free(sigtarget);
	return rc;
    }

    /* Now set the real archive size in the Header */
    headerModifyEntry(header, RPMTAG_ARCHIVESIZE,
		      RPM_INT32_TYPE, &archiveSize, 1);
    lseek(fd, 0,  SEEK_SET);
    headerWrite(fd, header, HEADER_MAGIC_YES);

    close(fd);

    /* Open the output file */
    if ((fd = open(fileName, O_WRONLY|O_CREAT|O_TRUNC, 0644)) == -1) {
	rpmError(RPMERR_CREATE, "Could not open %s\n", fileName);
	unlink(sigtarget);
	free(sigtarget);
	return RPMERR_CREATE;
    }

    /* Now write the lead */
    headerGetEntry(header, RPMTAG_NAME, NULL, (void **)&name, NULL);
    headerGetEntry(header, RPMTAG_VERSION, NULL, (void **)&version, NULL);
    headerGetEntry(header, RPMTAG_RELEASE, NULL, (void **)&release, NULL);
    sprintf(buf, "%s-%s-%s", name, version, release);
    rpmGetArchInfo(NULL, &arch);
    rpmGetOsInfo(NULL, &os);
    lead.major = RPM_MAJOR_NUMBER;
    lead.minor = 0;
    lead.type = type;
    lead.archnum = arch;
    lead.osnum = os;
    lead.signature_type = RPMSIG_HEADERSIG;  /* New-style signature */
    strncpy(lead.name, buf, sizeof(lead.name));
    if (writeLead(fd, &lead)) {
	rpmError(RPMERR_NOSPACE, "Unable to write package: %s",
		 strerror(errno));
	close(fd);
	unlink(sigtarget);
	free(sigtarget);
	unlink(fileName);
	return rc;
    }

    /* Generate the signature */
    sigtype = rpmLookupSignatureType();
    fflush(stdout);
    sig = rpmNewSignature();
    rpmAddSignature(sig, sigtarget, RPMSIGTAG_SIZE, passPhrase);
    rpmAddSignature(sig, sigtarget, RPMSIGTAG_MD5, passPhrase);
    if (sigtype > 0) {
	rpmMessage(RPMMESS_NORMAL, "Generating signature: %d\n", sigtype);
	rpmAddSignature(sig, sigtarget, sigtype, passPhrase);
    }
    if ((rc = rpmWriteSignature(fd, sig))) {
	close(fd);
	unlink(sigtarget);
	free(sigtarget);
	unlink(fileName);
	rpmFreeSignature(sig);
	return rc;
    }
    rpmFreeSignature(sig);
	
    /* Append the header and archive */
    ifd = open(sigtarget, O_RDONLY);
    while ((count = read(ifd, buf, sizeof(buf))) > 0) {
	if (count == -1) {
	    rpmError(RPMERR_READERROR, "Unable to read sigtarget: %s",
		     strerror(errno));
	    close(ifd);
	    close(fd);
	    unlink(sigtarget);
	    free(sigtarget);
	    unlink(fileName);
	    return RPMERR_READERROR;
	}
	if (write(fd, buf, count) < 0) {
	    rpmError(RPMERR_NOSPACE, "Unable to write package: %s",
		     strerror(errno));
	    close(ifd);
	    close(fd);
	    unlink(sigtarget);
	    free(sigtarget);
	    unlink(fileName);
	    return RPMERR_NOSPACE;
	}
    }
    close(ifd);
    close(fd);
    unlink(sigtarget);
    free(sigtarget);

    rpmMessage(RPMMESS_NORMAL, "Wrote: %s\n", fileName);

    return 0;
}

static int cpio_gzip(int fd, struct cpioFileMapping *cpioList,
		     int cpioCount, int *archiveSize)
{
    char *gzipbin;
    void *oldhandler;
    int rc, gzipPID, toGzip[2], status;
    char *failedFile;

    gzipbin = rpmGetVar(RPMVAR_GZIPBIN);
    oldhandler = signal(SIGPIPE, SIG_IGN);

    /* GZIP */
    pipe(toGzip);
    if (!(gzipPID = fork())) {
	close(toGzip[1]);
	
	dup2(toGzip[0], 0);  /* Make stdin the in pipe       */
	dup2(fd, 1);         /* Make stdout the passed-in fd */

	execlp(gzipbin, gzipbin, "-c9fn", NULL);
	rpmError(RPMERR_EXEC, "Couldn't exec gzip");
	_exit(RPMERR_EXEC);
    }
    close(toGzip[0]);
    if (gzipPID < 0) {
	close(toGzip[1]);
	rpmError(RPMERR_FORK, "Couldn't fork gzip");
	return RPMERR_FORK;
    }

    rc = cpioBuildArchive(toGzip[1], cpioList, cpioCount, NULL, NULL,
			  archiveSize, &failedFile);

    close(toGzip[1]);
    signal(SIGPIPE, oldhandler);
    waitpid(gzipPID, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
	rpmError(RPMERR_GZIP, "Execution of gzip failed");
	return 1;
    }

    if (rc) {
	if (rc & CPIO_CHECK_ERRNO)
	    rpmError(RPMERR_CPIO, "cpio failed on file %s: %s",
		     failedFile, strerror(errno));
	else
	    rpmError(RPMERR_CPIO, "cpio failed on file %s: %d",
		     failedFile, rc);
	return 1;
    }

    return 0;
}

static int addFileToTag(Spec spec, char *file, Header h, int tag)
{
    StringBuf sb;
    char *s;
    char buf[BUFSIZ];
    FILE *f;

    sb = newStringBuf();
    if (headerGetEntry(h, tag, NULL, (void **)&s, NULL)) {
	appendLineStringBuf(sb, s);
	headerRemoveEntry(h, tag);
    }
    sprintf(buf, "%s/%s/%s", rpmGetVar(RPMVAR_BUILDDIR),
	    spec->buildSubdir, file);
    if ((f = fopen(buf, "r")) == NULL) {
	freeStringBuf(sb);
	return 1;
    }
    while (fgets(buf, sizeof(buf), f)) {
	expandMacros(&spec->macros, buf);
	appendStringBuf(sb, buf);
    }
    fclose(f);
    
    headerAddEntry(h, tag, RPM_STRING_TYPE, getStringBuf(sb), 1);

    freeStringBuf(sb);
    return 0;
}

static int processScriptFiles(Spec spec, Package pkg)
{
    if (pkg->preInFile) {
	if (addFileToTag(spec, pkg->preInFile, pkg->header, RPMTAG_PREIN)) {
	    rpmError(RPMERR_BADFILENAME,
		     "Could not open PreIn file: %s", pkg->preInFile);
	    return RPMERR_BADFILENAME;
	}
    }
    if (pkg->preUnFile) {
	if (addFileToTag(spec, pkg->preUnFile, pkg->header, RPMTAG_PREUN)) {
	    rpmError(RPMERR_BADFILENAME,
		     "Could not open PreUn file: %s", pkg->preUnFile);
	    return RPMERR_BADFILENAME;
	}
    }
    if (pkg->postInFile) {
	if (addFileToTag(spec, pkg->postInFile, pkg->header, RPMTAG_POSTIN)) {
	    rpmError(RPMERR_BADFILENAME,
		     "Could not open PostIn file: %s", pkg->postInFile);
	    return RPMERR_BADFILENAME;
	}
    }
    if (pkg->postUnFile) {
	if (addFileToTag(spec, pkg->postUnFile, pkg->header, RPMTAG_POSTUN)) {
	    rpmError(RPMERR_BADFILENAME,
		     "Could not open PostUn file: %s", pkg->postUnFile);
	    return RPMERR_BADFILENAME;
	}
    }
    if (pkg->verifyFile) {
	if (addFileToTag(spec, pkg->verifyFile, pkg->header,
			 RPMTAG_VERIFYSCRIPT)) {
	    rpmError(RPMERR_BADFILENAME,
		     "Could not open VerifyScript file: %s", pkg->verifyFile);
	    return RPMERR_BADFILENAME;
	}
    }

    return 0;
}
