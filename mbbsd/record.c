/* $Id$ */

#include "bbs.h"

#define BUFSIZE 512

#define safewrite       write

int
get_sum_records(const char *fpath, int size)
{
    struct stat     st;
    int             ans = 0;
    FILE           *fp;
    fileheader_t    fhdr;
    char            buf[PATHLEN], *p;
   
    // Ptt : should avoid big loop
    if ((fp = fopen(fpath, "r"))==NULL)
	return -1;
    
    strlcpy(buf, fpath, sizeof(buf));
    p = strrchr(buf, '/');
    assert(p);
    p++;

    while (fread(&fhdr, size, 1, fp)==1) {
	strlcpy(p, fhdr.filename, sizeof(buf) - (p - buf));
	if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1)
	    ans += st.st_size;
    }
    fclose(fp);
    return ans / 1024;
}

/* return index>0 if thisstamp==stamp[index], 
 * return -index<0 if stamp[index-1]<thisstamp<stamp[index+1], XXX thisstamp ?<>? stamp[index]
 * 			or XXX filename[index]=""
 * return 0 if error
 */
int
getindex_m(const char *direct, fileheader_t *fhdr, int end, int isloadmoney)
{ // Ptt: 從前面找很費力 太暴力
    int             fd = -1, begin = 1, i, s, times, stamp;
    fileheader_t    fh;

    int n = get_num_records(direct, sizeof(fileheader_t));
    if( end > n || end<=0 )
           end = n;
    stamp = atoi(fhdr->filename + 2);
    for( i = (begin + end ) / 2, times = 0 ;
	 end >= begin  && times < 20    ; /* 最多只找 20 次 */
	 i = (begin + end ) / 2, ++times ){
        if( get_record_keep(direct, &fh, sizeof(fileheader_t), i, &fd)==-1 ||
	    !fh.filename[0] )
              break;
	s = atoi(fh.filename + 2);
	if( s > stamp )
	    end = i - 1;
	else if( s == stamp ){
	    close(fd);
	    if(isloadmoney)
 	       fhdr->multi.money = fh.multi.money; 
	    return i;
	}
        else
	    begin = i + 1; 
    }

    if( times < 20)     // Not forever loop. It any because of deletion.
	{
	   close(fd);
           return -i;
	}
    if( fd != -1 )
	close(fd);
    return 0;
}

inline int
getindex(const char *direct, fileheader_t *fhdr, int end)
{
  return getindex_m(direct, fhdr, end, 0);
}

int
substitute_ref_record(const char *direct, fileheader_t * fhdr, int ent)
{
    fileheader_t    hdr;
    char            fname[PATHLEN];
    int             num = 0;

    /* rocker.011018: 串接模式用reference增進效率 */
    if (!(fhdr->filemode & FILE_BOTTOM) &&  (fhdr->multi.refer.flag) &&
	    (num = fhdr->multi.refer.ref)){
	setdirpath(fname, direct, FN_DIR);
	get_record(fname, &hdr, sizeof(hdr), num);
	if (strcmp(hdr.filename, fhdr->filename)) {
	    if((num = getindex_m(fname, fhdr, num, 1))>0) {
		substitute_record(fname, fhdr, sizeof(*fhdr), num);
	    }
	}
	else if(num>0) {
	    fhdr->multi.money = hdr.multi.money;
	    substitute_record(fname, fhdr, sizeof(*fhdr), num);
	}
	fhdr->multi.refer.flag = 1;
	fhdr->multi.refer.ref = num; // Ptt: update now!
    }
    substitute_record(direct, fhdr, sizeof(*fhdr), ent);
    return num;
}


/* rocker.011022: 避免lock檔開啟時不正常斷線,造成永久lock */
#ifndef _BBS_UTIL_C_
int
force_open(const char *fname)
{
    int             fd;
    time4_t          expire;

    expire = now - 3600;	/* lock 存在超過一個小時就是有問題! */

    if (dasht(fname) > expire)
	return -1;
    unlink(fname);
    fd = open(fname, O_WRONLY | O_TRUNC, 0644);

    return fd;
}

/* new/old/lock file processing */
typedef struct nol_t {
    char            newfn[PATHLEN];
    char            oldfn[PATHLEN];
    char            lockfn[PATHLEN];
}               nol_t;

static void
nolfilename(nol_t * n, const char *fpath)
{
    snprintf(n->newfn, sizeof(n->newfn), "%s.new", fpath);
    snprintf(n->oldfn, sizeof(n->oldfn), "%s.old", fpath);
    snprintf(n->lockfn, sizeof(n->lockfn), "%s.lock", fpath);
}

#ifdef SAFE_ARTICLE_DELETE
void safe_delete_range(const char *fpath, int id1, int id2)
{
    int     fd, i;
    fileheader_t    fhdr;
    char            fullpath[STRLEN], *t;
    strlcpy(fullpath, fpath, sizeof(fullpath));
    t = strrchr(fullpath, '/');
    assert(t);
    t++;
    if( (fd = open(fpath, O_RDONLY)) == -1 )
	return;
    for( i = 1 ; (read(fd, &fhdr, sizeof(fileheader_t)) ==
		  sizeof(fileheader_t)) ; ++i ){
	strcpy(t, fhdr.filename);
	/* rocker.011018: add new tag delete */
	if (!((fhdr.filemode & FILE_MARKED) ||	/* 標記 */
	      (fhdr.filemode & FILE_DIGEST) ||	/* 文摘 */
	      (id1 && (i < id1 || i > id2)) ||	/* range */
	      (!id1 && Tagger(atoi(t + 2), i, TAG_NIN)))) 	/* TagList */
	    safe_article_delete(i, &fhdr, fpath);
    }
    close(fd);
}
#endif

int
delete_range(const char *fpath, int id1, int id2)
{
    fileheader_t    fhdr;
    nol_t           my;
    char            fullpath[STRLEN], *t;
    int             fdr, fdw, fd;
    int             count, dcount=0;

    nolfilename(&my, fpath);

    if ((fd = open(my.lockfn, O_RDWR | O_CREAT | O_APPEND, 0644)) == -1)
	return -1;

    flock(fd, LOCK_EX);

    if ((fdr = open(fpath, O_RDONLY, 0)) == -1) {
	flock(fd, LOCK_UN);
	close(fd);
	return -1;
    }
    if (
	((fdw = open(my.newfn, O_WRONLY | O_CREAT | O_EXCL, 0644)) == -1) &&
	((fdw = force_open(my.newfn)) == -1)) {
	close(fdr);
	flock(fd, LOCK_UN);
	close(fd);
	return -1;
    }
    count = 1;
    strlcpy(fullpath, fpath, sizeof(fullpath));
    t = strrchr(fullpath, '/');
    assert(t);
    t++;

    while (read(fdr, &fhdr, sizeof(fileheader_t)) == sizeof(fileheader_t)) {
	strcpy(t, fhdr.filename);

	/* rocker.011018: add new tag delete */
	if (
	    (fhdr.filemode & FILE_MARKED) ||	/* 標記 */
	    ((fhdr.filemode & FILE_DIGEST) && (currstat != RMAIL) )||	
	    /* 文摘 , FILE_DIGEST is used as REPLIED in mail menu.*/
	    (id1 && (count < id1 || count > id2)) ||	/* range */
	    (!id1 && Tagger(atoi(t + 2), count, TAG_NIN))) {	/* TagList */
	    if ((safewrite(fdw, &fhdr, sizeof(fileheader_t)) == -1)) {
		close(fdr);
		close(fdw);
		unlink(my.newfn);
		flock(fd, LOCK_UN);
		close(fd);
		return -1;
	    }
	} else {
	    //if (dashd(fullpath))
		unlink(fullpath);
                dcount++;
	}
	++count;
    }
    close(fdr);
    close(fdw);
    if (Rename(fpath, my.oldfn) == -1 || Rename(my.newfn, fpath) == -1) {
	flock(fd, LOCK_UN);
	close(fd);
	return -1;
    }
    flock(fd, LOCK_UN);
    close(fd);
    return dcount;
}
#endif // _BBS_UTIL_C_

void 
set_safedel_fhdr(fileheader_t *fhdr)
{
    if (fhdr->filemode & FILE_ANONYMOUS ||
	!fhdr->owner[0] ||
	(fhdr->owner[0] == '-' && fhdr->owner[1] == 0) )
    {
	// not putting owner because we can't know
	// if it is deleted by BM or owner.
	snprintf(fhdr->title, sizeof(fhdr->title),
		"%s", STR_SAFEDEL_TITLE);
    }
    else if ( strcmp(fhdr->owner, cuser.userid) == 0 )
    {
	// i'm the one to delete it
	snprintf(fhdr->title, sizeof(fhdr->title),
		"%s [%s]", STR_SAFEDEL_TITLE, fhdr->owner);
    } 
    else // deleted by BM, system, SYSOP, or other services...
	// maybe not revealing the names would be better.
    {
	snprintf(fhdr->title, sizeof(fhdr->title),
		"%s <%s>", STR_SAFEDEL_TITLE, fhdr->owner);
    }
    // snprintf(fhdr->title, sizeof(fhdr->title), "%s", STR_SAFEDEL_TITLE);
    strcpy(fhdr->filename, FN_SAFEDEL);
    strcpy(fhdr->owner, "-");
}

#ifdef SAFE_ARTICLE_DELETE
int
safe_article_delete(int ent, const fileheader_t *fhdr, const char *direct)
{
    fileheader_t newfhdr;
    memcpy(&newfhdr, fhdr, sizeof(fileheader_t));
    set_safedel_fhdr(&newfhdr);
    substitute_record(direct, &newfhdr, sizeof(newfhdr), ent);
    return 0;
}

int
safe_article_delete_range(const char *direct, int from, int to)
{
    fileheader_t newfhdr;
    int     fd;
    char    fn[128], *ptr;

    strlcpy(fn, direct, sizeof(fn));
    if( (ptr = rindex(fn, '/')) == NULL )
	return -1;

    ++ptr;
    if( (fd = open(direct, O_RDWR)) != -1 &&
	lseek(fd, sizeof(fileheader_t) * (from - 1), SEEK_SET) != -1 ){

	for( ; from <= to ; ++from ){
	    read(fd, &newfhdr, sizeof(fileheader_t));
	    if( newfhdr.filemode & (FILE_MARKED | FILE_DIGEST) )
		continue;
	    if(newfhdr.filename[0]=='L') newfhdr.filename[0]='M';
	    strlcpy(ptr, newfhdr.filename, sizeof(newfhdr.filename));
	    unlink(fn);

	    set_safedel_fhdr(&newfhdr);
	    // because off_t is unsigned, we could NOT seek backward.
	    lseek(fd, sizeof(fileheader_t) * (from - 1), SEEK_SET);
	    write(fd, &newfhdr, sizeof(fileheader_t));
	}
	close(fd);
	return 0;
    }
    return -1;
}


#endif		

/* mail / post 時，依據時間建立檔案，加上郵戳 */
int
stampfile_u(char *fpath, fileheader_t * fh) 
  // Ptt: stampfile_u: won't clear fileheader
  //      stampfile: will clear fileheader
{
    register char  *ip = fpath;
    time4_t         dtime = COMMON_TIME;
    struct tm       ptime;
#ifdef _BBS_UTIL_C_
    int             fp = 0;  //Ptt: don't need to check 
    // for utils, the time may be the same between several runs, by scw
#endif

    if (access(fpath, X_OK | R_OK | W_OK))
	mkdir(fpath, 0755);

    while (*(++ip));
    *ip++ = '/';
#ifdef _BBS_UTIL_C_
    do {
#endif
	sprintf(ip, "M.%d.A.%3.3X", (int)(++dtime), (unsigned int)(random() & 0xFFF));
#ifdef _BBS_UTIL_C_
	if (fp == -1 && errno != EEXIST)
	    return -1;
   } while ((fp = open(fpath, O_CREAT | O_EXCL | O_WRONLY, 0644)) == -1);
    close(fp);
#endif
    strlcpy(fh->filename, ip, sizeof(fh->filename));
    localtime4_r(&dtime, &ptime);
    snprintf(fh->date, sizeof(fh->date),
	     "%2d/%02d", ptime.tm_mon + 1, ptime.tm_mday);
    return 0;
}

inline int
stampfile(char *fpath, fileheader_t * fh)
{
  memset(fh, 0, sizeof(fileheader_t));
  return stampfile_u(fpath, fh);
}

int
stampdir(char *fpath, fileheader_t * fh)
{
    register char  *ip = fpath;
    time4_t          dtime = COMMON_TIME;
    struct tm      ptime;

    if (access(fpath, X_OK | R_OK | W_OK))
	mkdir(fpath, 0755);

    while (*(++ip));
    *ip++ = '/';
    do {
	sprintf(ip, "D%X", (int)++dtime & 07777);
    } while (mkdir(fpath, 0755) == -1);
    memset(fh, 0, sizeof(fileheader_t));
    strlcpy(fh->filename, ip, sizeof(fh->filename));
    localtime4_r(&dtime, &ptime);
    snprintf(fh->date, sizeof(fh->date),
	     "%2d/%02d", ptime.tm_mon + 1, ptime.tm_mday);

    return 0;
}

int
append_record_forward(char *fpath, fileheader_t * record, int size, const char *origid)
{
// #ifdef USE_MAIL_AUTO_FORWARD
#if !defined(_BBS_UTIL_C_)
    if (get_num_records(fpath, sizeof(fileheader_t)) <= MAX_KEEPMAIL * 2) {
	FILE           *fp;
	char            buf[512];
	int             n;

	for (n = strlen(fpath) - 1; fpath[n] != '/' && n > 0; n--);
	if (n + sizeof(".forward") > sizeof(buf))
	    return -1;
	memcpy(buf, fpath, n+1);
	strcpy(buf + n + 1, ".forward");
	if ((fp = fopen(buf, "r"))) {

	    char address[64];
	    int flIdiotSent2Self = 0;
	    int oidlen = origid ? strlen(origid) : 0;

	    address[0] = 0;
	    fscanf(fp, "%63s", address);
	    fclose(fp);
	    /* some idiots just set forwarding to themselves.
	     * and even after we checked "sameid", some still
	     * set STUPID_ID.bbs@host <- "自以為聰明"
	     * damn it, we have a complex rule now.
	     */
	    if(oidlen > 0) {
		if (strncasecmp(address, origid, oidlen) == 0)
		{
		    int addrlen = strlen(address);
		    if(	addrlen == oidlen ||
			(addrlen > oidlen && 
			 strcasecmp(address + oidlen, str_mail_address) == 0))
			flIdiotSent2Self = 1;
		}
	    }

	    if (buf[0] && buf[0] != ' ' && !flIdiotSent2Self) {
		buf[n + 1] = 0;
		strcat(buf, record->filename);
		append_record(fpath, record, size);
		// because too many user set wrong forward address,
		// let's but them instead. 
		bsmtp(buf, record->title, address, origid);
		return 0;
	    }
	}
    }
#endif
// #endif // USE_MAIL_AUTO_FORWARD

    append_record(fpath, record, size);

    return 0;
}

#ifndef _BBS_UTIL_C_
void 
setaidfile(char *buf, const char *bn, aidu_t aidu)
{
    // try to load by AID
    int bid = 0;
    int n = 0, fd = 0;
    char bfpath[PATHLEN];
    boardheader_t  *bh = NULL;
    fileheader_t fh;

    buf[0] = 0;
    bid = getbnum(bn);

    if (bid <= 0) return;
    assert(0<=bid-1 && bid-1<MAX_BOARD);
    bh = getbcache(bid);
    if (!HasBoardPerm(bh)) return;

    setbfile(bfpath, bh->brdname, FN_DIR);
    n = search_aidu(bfpath, aidu);

    if (n < 0) return;
    fd = open(bfpath, O_RDONLY);
    if (fd < 0) return;

    lseek(fd, n*sizeof(fileheader_t), SEEK_SET);
    memset(&fh, 0, sizeof(fh));
    if (read(fd, &fh, sizeof(fh)) > 0)
    {
	setbfile(buf, bh->brdname, fh.filename);
	if (!dashf(buf))
	    buf[0] = 0;
    }
    close(fd);
}
#endif
