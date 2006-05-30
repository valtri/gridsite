/*
   Copyright (c) 2003-6, Andrew McNab,
   University of Manchester. All rights reserved.

   Redistribution and use in source and binary forms, with or
   without modification, are permitted provided that the following
   conditions are met:

     o Redistributions of source code must retain the above
       copyright notice, this list of conditions and the following
       disclaimer. 
     o Redistributions in binary form must reproduce the above
       copyright notice, this list of conditions and the following
       disclaimer in the documentation and/or other materials
       provided with the distribution. 

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/*------------------------------------------------------------------*
 * This program is part of GridSite: http://www.gridsite.org/       *
 *------------------------------------------------------------------*/

#define _XOPEN_SOURCE

#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>              
#include <dirent.h>
#include <malloc.h>
#include <curl/curl.h>
#include <pthread.h>

#include <fuse.h>

#define GRST_SLASH_PIDFILE "/var/run/slashgrid.pid"

#define GRST_SLASH_HEADERS "/var/spool/slashgrid/headers"
#define GRST_SLASH_BLOCKS  "/var/spool/slashgrid/blocks"
#define GRST_SLASH_TMP     "/var/spool/slashgrid/tmp"
#define GRST_SLASH_DIRFILE "::DIR::"

#define GRST_SLASH_HEAD   0
#define GRST_SLASH_GET    1
#define GRST_SLASH_PUT    2
#define GRST_SLASH_DELETE 3
#define GRST_SLASH_MOVE   4
#define GRST_SLASH_TRUNC  5

#define GRST_SLASH_HEADERS_EXPIRE	60
#define GRST_SLASH_BLOCK_SIZE		4096
#define GRST_SLASH_MAX_HANDLES		16

#ifndef CURLOPT_WRITEDATA
#define CURLOPT_WRITEDATA CURLOPT_FILE
#endif
 
#ifndef CURLOPT_READDATA
#define CURLOPT_READDATA CURLOPT_FILE
#endif

#ifndef CURLE_HTTP_RETURNED_ERROR
#define CURLE_HTTP_RETURNED_ERROR CURLE_HTTP_NOT_FOUND
#endif

struct grst_body_text { char   *text;
                        size_t  used;
                        size_t  allocated; } ;

struct grst_read_data { const char  *buf;
                        off_t sent; 
                        off_t maxsent; };

struct grst_dir_list { char   *filename;
                       off_t   length;
                       int     length_set;
                       time_t  modified;
                       int     modified_set; } ;

struct grst_request { int     retcode;                         
                      char   *location;
                      size_t  length;
                      int     length_set;
                      time_t  modified;                           
                      int     modified_set; 
                      void   *readfunction;
                      void   *readdata;
                      void   *writefunction;
                      void   *writedata;
                      size_t  infilesize;
                      char   *errorbuffer;
                      char   *url;
                      int     method;
                      char   *destination;
                      off_t   start;
                      off_t   finish; } ;

struct grst_handle { pthread_mutex_t	mutex;
                     CURL		*curl_handle;
                     uid_t		uid;
                     char		*proxyfile;
                     time_t		last_used;
                   }  handles[GRST_SLASH_MAX_HANDLES];
 
int debugmode = 0;

size_t headers_callback(void *ptr, size_t size, size_t nmemb, void *p)
/* Find the values of the return code, Content-Length, Last-Modified
   and Location headers */
{
  float f;
  char  *s, *q;
  size_t realsize;
  struct tm modified_tm;
  struct grst_request *request_data = (struct grst_request *) p;

  realsize = size * nmemb;
  s = malloc(realsize + 1);
  memcpy(s, ptr, realsize);
  s[realsize] = '\0';

  if      (sscanf(s, "Content-Length: %d", &(request_data->length)) == 1) 
            request_data->length_set = 1;
  else if (sscanf(s, "HTTP/%f %d ", &f, &(request_data->retcode)) == 2) ;
  else if (strncmp(s, "Location: ", 10) == 0) 
      {
        request_data->location = strdup(&s[10]);
        
        for (q=request_data->location; *q != '\0'; ++q)
         if ((*q == '\r') || (*q == '\n')) *q = '\0';
      }
  else if (strncmp(s, "Last-Modified: ", 15) == 0)
      {
        /* follow RFC 2616: first try RFC 822 (kosher), then RFC 850 and 
           asctime() formats too. Must be GMT whatever the format. */

        if (strptime(&s[15], "%a, %d %b %Y %T GMT", &modified_tm) != NULL)
          {
            request_data->modified = mktime(&modified_tm);
            request_data->modified_set = 1;
          }
        else if (strptime(&s[15], "%a, %d-%b-%y %T GMT", &modified_tm) != NULL)
          {
            request_data->modified = mktime(&modified_tm);
            request_data->modified_set = 1;
          }
        else if (strptime(&s[15], "%a %b %d %T %Y", &modified_tm) != NULL)
          {
            request_data->modified = mktime(&modified_tm);
            request_data->modified_set = 1;
          }
      }
    
  free(s);
  return realsize;
}

int debug_callback(CURL *handle, curl_infotype infotype, 
                   char *rawmesg, size_t size, void *i)
{
  int   n;
  char *mesg;

  if ((infotype == CURLINFO_DATA_IN) ||
      (infotype == CURLINFO_DATA_OUT)) return 0;

  mesg = malloc(size + 1);
  
  for (n=0; n < size; ++n)
     {
       if ((rawmesg[n] == '\r') && (n >= size - 2)) mesg[n] = '\0';
       else if (((rawmesg[n] == '\r') || (rawmesg[n] == '\n')) && 
                (infotype == CURLINFO_HEADER_IN)) mesg[n] = '<';
       else if (((rawmesg[n] == '\r') || (rawmesg[n] == '\n')) && 
                (infotype == CURLINFO_HEADER_OUT)) mesg[n] = '>';
       else if ((rawmesg[n] < ' ') || (rawmesg[n] >= 127)) mesg[n] = '.';
       else mesg[n] = rawmesg[n];
     }
     
  mesg[n] = '\0';

  syslog(LOG_DEBUG, "%d %s%s%s%s", 
                    *((int *) i), 
                    (infotype == CURLINFO_HEADER_IN ) ? "<<" : "",
                    (infotype == CURLINFO_HEADER_OUT) ? ">>" : "",
                    (infotype == CURLINFO_TEXT      ) ? "**" : "",
                    mesg);

  free(mesg);  
  return 0;
}                  

char *check_x509_user_proxy(pid_t pid)
{
  int fd;
  char file[80], *proxyfile = NULL, *pid_environ, *p;
  struct stat statbuf1, statbuf2;
  
  snprintf(file, sizeof(file), "/proc/%d/environ", (int) pid);
  
  if ((fd = open(file, O_RDONLY)) == -1) return NULL;

  if (debugmode) syslog(LOG_DEBUG, "Opened for %d environ in %s", (int) pid, file);
  
  fstat(fd, &statbuf1);
  
  pid_environ = malloc(statbuf1.st_size + 1);
  
  read(fd, pid_environ, statbuf1.st_size);
  
  close(fd);
  
  pid_environ[statbuf1.st_size] = '\0';
    
  for (p = pid_environ; p < pid_environ + statbuf1.st_size; p += (strlen(p) + 1))
     {
       if (debugmode) syslog(LOG_DEBUG, "Examine %s in environ", p);
  
       if (strncmp(p, "X509_USER_PROXY=", 16) == 0)
         {
           if ((p[16] != '\0') &&
               (stat(&p[16], &statbuf2) == 0)) proxyfile = strdup(&p[16]);
           break;
         }
     }
  
  free(pid_environ);

  return proxyfile;    
}

int perform_request(struct grst_request *request_data,
                    struct fuse_context *fuse_ctx)
{
  int                ret, i, j;
  char              *proxyfile = NULL, *range_header = NULL;
  struct stat        statbuf;
  struct curl_slist *headers_list = NULL;

  if (strncmp(request_data->url, "https://", 8) == 0) /* HTTPS options */
    {
// check for X509_USER_PROXY in that PID's environ too

      if ((proxyfile = check_x509_user_proxy(fuse_ctx->pid)) == NULL)
        {
          asprintf(&proxyfile, "/tmp/x509up_u%d", fuse_ctx->uid);
          /* if proxyfile is used, it will be referenced by handles[].proxyfile
             and freed when this handle is eventually freed */

          if ((stat(proxyfile, &statbuf) != 0) ||
              (statbuf.st_uid != fuse_ctx->uid))
            {
              free(proxyfile);
              proxyfile = NULL;
            }
        }
    }

  if (debugmode && (proxyfile != NULL))
       syslog(LOG_DEBUG, "Using proxy file %s", proxyfile);

  /* try to find an existing handle for this uid/proxyfile */

  for (i=0; i < GRST_SLASH_MAX_HANDLES; ++i)
     {
       if ((handles[i].curl_handle != NULL)  &&
           (handles[i].uid == fuse_ctx->uid) &&
           (((handles[i].proxyfile == NULL) && (proxyfile == NULL)) ||
            ((handles[i].proxyfile != NULL) && (proxyfile != NULL) &&
             (strcmp(handles[i].proxyfile, proxyfile) == 0))))
         {
           break;
         }
     }
     
  if (i >= GRST_SLASH_MAX_HANDLES) /* no existing match found */
    {
      i=0;
    
      for (j=0; j < GRST_SLASH_MAX_HANDLES; ++j)
         {
           if (handles[j].curl_handle == NULL) /* unused slot */
             {
               i = j;
               break;
             }
             
           if (handles[j].last_used < handles[i].last_used) i = j;
         }
    }

  /* now lock this handle and recheck settings inside the mutex lock */

  pthread_mutex_lock(&(handles[i].mutex));

  if ((handles[i].curl_handle == NULL)  ||
      (handles[i].uid != fuse_ctx->uid) ||
      (((handles[i].proxyfile != NULL) || (proxyfile != NULL)) &&
       ((handles[i].proxyfile == NULL) || (proxyfile == NULL) ||
        (strcmp(handles[i].proxyfile, proxyfile) != 0))))
    {
      /* we do need to initialise this handle */
      
      handles[i].uid = fuse_ctx->uid;

      if (handles[i].curl_handle != NULL)
                              curl_easy_cleanup(handles[i].curl_handle);
      handles[i].curl_handle = curl_easy_init();
      
      if (handles[i].proxyfile != NULL) free(handles[i].proxyfile);
      handles[i].proxyfile = proxyfile; /* proxyfile might be NULL itself */
      
      if (handles[i].proxyfile != NULL)
        {
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSLCERTTYPE, "PEM");
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSLCERT,
                                                    handles[i].proxyfile);
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSLKEYTYPE, "PEM");
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSLKEY,  
                                                    handles[i].proxyfile);
        }
      else
        {
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSLKEYTYPE,  "ENG");
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSLCERTTYPE, "ENG");
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSLCERT,     NULL);
        }

      if (debugmode)
        {
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_VERBOSE, 1);
          curl_easy_setopt(handles[i].curl_handle, CURLOPT_DEBUGFUNCTION,
                                                          debug_callback);
        }

      curl_easy_setopt(handles[i].curl_handle, CURLOPT_USERAGENT, 
                       "SlashGrid http://www.gridsite.org/slashgrid/");
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_FOLLOWLOCATION, 0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_HEADERFUNCTION, headers_callback);

      curl_easy_setopt(handles[i].curl_handle, CURLOPT_CAPATH, 
                                        "/etc/grid-security/certificates");

      curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSL_VERIFYPEER, 2);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_SSL_VERIFYHOST, 2);
    }   
    
  if (request_data->method == GRST_SLASH_HEAD)
    {
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_CUSTOMREQUEST, NULL);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_NOBODY,  1);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_HTTPGET, 0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_UPLOAD,  0);
    }
  else if (request_data->method == GRST_SLASH_GET)
    {
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_CUSTOMREQUEST, NULL);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_NOBODY,  0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_HTTPGET, 1);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_UPLOAD,  0);
    }
  else if ((request_data->method == GRST_SLASH_PUT) || 
           (request_data->method == GRST_SLASH_TRUNC))
    {
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_CUSTOMREQUEST, NULL);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_NOBODY,  0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_HTTPGET, 0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_UPLOAD,  1);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_INFILESIZE,  
                                            (long) request_data->infilesize);
    }
  else if (request_data->method == GRST_SLASH_DELETE)
    {
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_NOBODY,  0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_HTTPGET, 0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_UPLOAD,  0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
  else if (request_data->method == GRST_SLASH_MOVE)
    {
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_NOBODY,  0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_HTTPGET, 0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_UPLOAD,  0);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_CUSTOMREQUEST, "MOVE");
    }
  else return CURLE_UNSUPPORTED_PROTOCOL;

  curl_easy_setopt(handles[i].curl_handle, CURLOPT_WRITEHEADER,    request_data);
  
  if (request_data->errorbuffer != NULL)
        curl_easy_setopt(handles[i].curl_handle, CURLOPT_ERRORBUFFER,
                                      request_data->errorbuffer);

  if (debugmode)
        curl_easy_setopt(handles[i].curl_handle, CURLOPT_DEBUGDATA, &i);

  curl_easy_setopt(handles[i].curl_handle, CURLOPT_READFUNCTION, request_data->readfunction);
  curl_easy_setopt(handles[i].curl_handle, CURLOPT_READDATA, request_data->readdata);
  curl_easy_setopt(handles[i].curl_handle, CURLOPT_WRITEFUNCTION, request_data->writefunction);
  curl_easy_setopt(handles[i].curl_handle, CURLOPT_WRITEDATA, request_data->writedata);

  curl_easy_setopt(handles[i].curl_handle, CURLOPT_URL, request_data->url);

  if ((request_data->start >= 0) && 
      (request_data->finish >= request_data->start))
    {
      if (request_data->method == GRST_SLASH_PUT)
           asprintf(&range_header, "Content-Range: bytes %ld-%ld/*", 
               (long) request_data->start, (long) request_data->finish);
      else if (request_data->method == GRST_SLASH_TRUNC)
           asprintf(&range_header, "Content-Range: bytes *-*/%ld", 
               (long) request_data->finish);
      else asprintf(&range_header, "Range: bytes=%ld-%ld", 
               (long) request_data->start, (long) request_data->finish);

      headers_list = curl_slist_append(headers_list, range_header);
      curl_easy_setopt(handles[i].curl_handle, CURLOPT_HTTPHEADER, headers_list);
    }
  else curl_easy_setopt(handles[i].curl_handle, CURLOPT_HTTPHEADER, NULL);

  ret = curl_easy_perform(handles[i].curl_handle);
  
  if (headers_list != NULL) curl_slist_free_all(headers_list);
  if (range_header != NULL) free(range_header);

  pthread_mutex_unlock(&(handles[i].mutex));
  
  return ret;
}

size_t rawbody_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
  if ( ((struct grst_body_text *) data)->used + size * nmemb >=
                             ((struct grst_body_text *) data)->allocated )
    {
      ((struct grst_body_text *) data)->allocated = 
        ((struct grst_body_text *) data)->used + size * nmemb + 4096;

      ((struct grst_body_text *) data)->text = 
         realloc( ((struct grst_body_text *) data)->text,
                  ((struct grst_body_text *) data)->allocated );
    }
    
  memcpy( &( ((struct grst_body_text *) 
                 data)->text[((struct grst_body_text *) data)->used] ),
          ptr, size * nmemb);
          
  ((struct grst_body_text *) data)->used += size * nmemb;
  
  return size * nmemb;
}

size_t null_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
  return size * nmemb;
}

size_t read_data_callback(void *ptr, size_t size, size_t nmemb, void *data)
{
  size_t sent;
  
  if (((struct grst_read_data *) data)->sent 
        >= ((struct grst_read_data *) data)->maxsent) return 0;
        
  if (size * nmemb + ((struct grst_read_data *) data)->sent 
        >= ((struct grst_read_data *) data)->maxsent)
    {
      sent = ((struct grst_read_data *) data)->maxsent 
             - ((struct grst_read_data *) data)->sent;
    }
  else sent = size * nmemb;        

  memcpy(ptr, 
         ((struct grst_read_data *) data)->buf +
         ((struct grst_read_data *) data)->sent,
         sent);

  ((struct grst_read_data *) data)->sent += sent;
  
  return sent;
}

char *canonicalise(char *link, char *source)
{
  int   i, j, srclen;
  char *s;

  srclen = strlen(source);

  if ((strncmp(link, "https://", 8) == 0) ||
      (strncmp(link, "http://", 7) == 0))
    {
      if (strncmp(link, source, srclen) != 0) return NULL; /* other site */
      
      if (link[srclen] == '\0') return NULL; /* we dont self-link! */
      
      for (i=0; link[srclen + i] != '\0'; ++i)
        if (link[srclen + i] == '/')
          { 
            if (link[srclen + i + 1] != '\0') return NULL; /* no subdirs */
            else return strdup(&link[srclen]); /* resolves to this dir */
          }
    }
  else if (link[0] != '/') /* relative link - need to check for subsubdirs */
    {
      for (i=0; link[i] != '\0'; ++i) 
        if ((link[i] == '/') && (link[i+1] != '\0')) return NULL;

      s = strdup(link);
      
      for (i=0; s[i] != '\0'; ++i) 
       if (s[i] == '#')
         {
           s[i] = '\0';
           break;
         }

      return s;
    }

  /* absolute link on this server, starting / */

  for (i=8; source[i] != '\0'; ++i) if (source[i] == '/') break;
       
  if (strncmp(link, &source[i], srclen - i) != 0) return NULL;

  for (j = srclen - i; link[j] != '\0'; ++j)
        if ((link[j] == '/') && (link[j+1] != '\0')) return NULL;
        
  s = strdup(&link[srclen - i]);
      
  for (i=0; s[i] != '\0'; ++i) 
       if (s[i] == '#')
         {
           s[i] = '\0';
           break;
         }

  if (s[0] == '\0') /* on second thoughts... */
    {
      free(s);
      return NULL;
    }
         
  return s;      
}

int grst_dir_list_cmp(const void *a, const void *b)
{
  return strcmp( ((struct grst_dir_list *) a)->filename, 
                 ((struct grst_dir_list *) b)->filename);
}

struct grst_dir_list *index_to_dir_list(char *text, char *source)
{
  int   taglevel = 0, wordnew = 1, i, namestart, used = 0, 
        allocated = 256;
  char *p, *s;
  struct grst_dir_list *list;
  
  list = (struct grst_dir_list *)
              malloc(allocated * sizeof(struct grst_dir_list));
              
  list[0].filename     = NULL;
  list[0].length       = 0;
  list[0].length_set   = 0;
  list[0].modified     = 0;
  list[0].modified_set = 0;
    
  for (p=text; *p != '\0'; ++p)
     {
       if (*p == '<') 
         {
           ++taglevel;
           
           if ((taglevel == 1) && (list[used].filename != NULL))
             {
               ++used;
               if (used >= allocated) 
                 {
                   allocated += 256;
                   list = (struct grst_dir_list *)
                           realloc((void *) list,
                                   allocated * sizeof(struct grst_dir_list));
                 }
                 
               list[used].filename     = NULL;
               list[used].length       = 0;
               list[used].length_set   = 0;
               list[used].modified     = 0;
               list[used].modified_set = 0;
             }

           wordnew = 1;
           continue;
         }

       if (*p == '>') 
         {
           --taglevel;
           wordnew = 1;
           continue;
         }
         
       if (isspace(*p))
         {
           wordnew = 1;
           continue;
         }

       if ((wordnew) && (taglevel == 1))
         {        
           if (((*p == 'h') || (*p == 'H')) && 
               (strncasecmp(p, "href=", 5) == 0))
             {
               if (p[5] == '"') { namestart = 6;
                                  for (i=namestart; (p[i] != '\0') &&
                                                    (p[i] != '"' ) &&
                                                    (p[i] != '\n') &&
                                                    (p[i] != '\t') &&
                                                    (p[i] != '>' ) ; ++i) ; }
               else { namestart = 5;
                      for (i=namestart; (p[i] != '\0') &&
                                        (p[i] != '"' ) &&
                                        (p[i] != ' ' ) &&
                                        (p[i] != '\n') &&
                                        (p[i] != '\t') &&
                                        (p[i] != ')' ) &&
                                        (p[i] != '>' ) ; ++i) ; }
               if (i > namestart) 
                 {
                   s = malloc(1 + i - namestart);
                   memcpy(s, &p[namestart], i - namestart);
                   s[i - namestart] = '\0';

                   list[used].filename = canonicalise(s, source);
                   free(s);
                 }
                 
               p = &p[i-1]; /* -1 since continue results in ++i */
               continue;
             }

           if (((*p == 'c') || (*p == 'C')) && 
               (strncasecmp(p, "content-length=", 15) == 0))
             {
               list[used].length     = 0;
               list[used].length_set = 1;
               
               if (p[15] == '"') list[used].length = atoi(&p[16]);
               else              list[used].length = atoi(&p[15]);

               p = &p[15];
               continue;
             }

           if (((*p == 'l') || (*p == 'L')) && 
               (strncasecmp(p, "last-modified=", 14) == 0))
             {
               list[used].modified     = 0;
               list[used].modified_set = 1;
               
               if (p[14] == '"') list[used].modified = atoi(&p[15]);
               else              list[used].modified = atoi(&p[14]);

               p = &p[14];
               continue;
             }
         }
         
       wordnew = 0;
     }  

  qsort((void *) list, used, sizeof(struct grst_dir_list), grst_dir_list_cmp);

  return list;  
}

static char *GRSThttpUrlMildencode(char *in)
/* Return a pointer to a malloc'd string holding a partially URL-encoded
   version of *in. "Partially" means that A-Z a-z 0-9 . = - _ @ and /
   are passed through unmodified. (DN's processed by GRSThttpUrlMildencode()
   can be used as valid Unix paths+filenames if you are prepared to
   create or simulate the resulting /X=xyz directories.) */
{
  char *out, *p, *q;

  out = malloc(3*strlen(in) + 1);

  p = in;
  q = out;

  while (*p != '\0')
       {
         if (isalnum(*p) || (*p == '.') || (*p == '=') || (*p == '-')
                         || (*p == '/') || (*p == '@') || (*p == '_'))
           {
             *q = *p;
             ++q;
           }
         else if (*p == ' ')
           {
             *q = '+';
             ++q;
           }
         else
           {
             sprintf(q, "%%%2X", *p);
             q = &q[3];
           }

         ++p;
       }

  *q = '\0';
  return out;
}

int read_headers_from_cache(struct fuse_context *fuse_ctx, char *filename, 
                            off_t *length, time_t *modified)
{
  char *encoded_filename, *disk_filename;
  int   len;
  long  content_length, last_modified;
  FILE *fp;
  struct stat statbuf;
  time_t now;
  
  encoded_filename = GRSThttpUrlMildencode(filename);
  
  len = strlen(encoded_filename);

  if (encoded_filename[len - 1] == '/') /* a directory */
       asprintf(&disk_filename, "%s/%d%s%s", 
                GRST_SLASH_HEADERS, fuse_ctx->uid, encoded_filename, GRST_SLASH_DIRFILE);
  else asprintf(&disk_filename, "%s/%d%s%s", 
                GRST_SLASH_HEADERS, fuse_ctx->uid, encoded_filename);

  free(encoded_filename);

// Change to fstat for the benefit of multiple threads:

  if (stat(disk_filename, &statbuf) != 0) /* no cache file to read */
    {
      free(disk_filename);
      return 0;
    }

  time(&now);

  if (statbuf.st_mtime < now - GRST_SLASH_HEADERS_EXPIRE)
    {
      unlink(disk_filename); /* tidy up expired cache file */
      free(disk_filename);
      return 0;
    }      

  last_modified  = 0;
  content_length = 0;

  if (debugmode) syslog(LOG_DEBUG, "Opening %s from cache", disk_filename);

  fp = fopen(disk_filename, "r");
  free(disk_filename);

  if (fp != NULL)
    {
      fscanf(fp, "content-length=%ld last-modified=%ld ", 
                 &content_length, &last_modified);
      fclose(fp);

      if (debugmode) syslog(LOG_DEBUG, "content-length=%ld last-modified=%ld", 
                            content_length, last_modified);

      *length   = (off_t)  content_length;
      *modified = (time_t) last_modified;

      return 1;
    }

  return 0;
}

int write_headers_to_cache(struct fuse_context *fuse_ctx, char *filename, 
                           off_t length, time_t modified)
{
  int         fd, len;
  char       *tempfile, *headline, *encoded_filename, *p, *newdir,
             *new_filename;
  struct stat statbuf;

  asprintf(&tempfile, "%s/headers-XXXXXX", GRST_SLASH_TMP);
  fd = mkstemp(tempfile);

  if (fd == -1)
    {
      free(tempfile);
      return 0;
    }

  asprintf(&headline, "content-length=%ld last-modified=%ld \n", 
                                (long) length, (long) modified);
  
  if ((write(fd, headline, strlen(headline)) == -1) ||
      (close(fd) == -1))
    {
      free(tempfile);
      free(headline);
      return 0;
    }

  free(headline);
                     
  encoded_filename = GRSThttpUrlMildencode(filename);

// need to protect against .. ?
   
  for (p = encoded_filename; *p != '\0'; ++p)
     {  
       if (*p != '/') continue;
     
       *p = '\0';
       asprintf(&newdir, "%s/%d%s", GRST_SLASH_HEADERS, fuse_ctx->uid, encoded_filename);
       *p = '/';
           
       if (stat(newdir, &statbuf) == 0)
         {
           if (!S_ISDIR(statbuf.st_mode)) /* exists already - not a directory! */
             {
               unlink(newdir);
               mkdir(newdir, S_IRUSR | S_IWUSR | S_IXUSR);
             }
           /* else it already exists as a directory - so ok */
         }
       else mkdir(newdir, S_IRUSR | S_IWUSR | S_IXUSR);

       free(newdir);
     }

  len = strlen(encoded_filename);

  if (encoded_filename[len - 1] == '/') /* a directory */
       asprintf(&new_filename, "%s/%d%s%s", 
                GRST_SLASH_HEADERS, fuse_ctx->uid, encoded_filename, GRST_SLASH_DIRFILE);
  else asprintf(&new_filename, "%s/%d%s", 
                GRST_SLASH_HEADERS, fuse_ctx->uid, encoded_filename);

  free(encoded_filename);
  
  if ((stat(new_filename, &statbuf) == 0) && S_ISDIR(statbuf.st_mode))
    {
// need change this to do it recursively in case any files/subdirs too
      rmdir(new_filename);
    }

  rename(tempfile, new_filename);

  if (debugmode) syslog(LOG_DEBUG, "Added %s to cache (%ld %ld)\n", 
                                   new_filename, length, modified);

  free(tempfile);
  free(new_filename);

  return 1;
}

static int slashgrid_readdir(const char *path, void *buf, 
                             fuse_fill_dir_t filler,
                             off_t offset, struct fuse_file_info *fi)
{
  (void) offset;
  (void) fi;

  int          anyerror = 0, thiserror, i, ilast;
  const char  *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", 
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char        *s, *url, errorbuffer[CURL_ERROR_SIZE+1] = "";
  struct       grst_body_text  rawindex;
  struct       grst_dir_list   *list;
  struct       grst_request request_data;
  struct       tm               modified_tm;
  struct       stat             stat_tmp;
  time_t                        now;
  struct fuse_context fuse_ctx;
  
  memcpy(&fuse_ctx, fuse_get_context(), sizeof(struct fuse_context));

  if (debugmode) syslog(LOG_DEBUG, "in slashgrid_readdir");

  if (strncmp(path, "/http/", 6) == 0)
    asprintf(&url, "http://%s/", &path[6]);
  else if (strncmp(path, "/https/", 7) == 0)
    asprintf(&url, "https://%s/", &path[7]);
  else return -ENOENT;

  rawindex.text      = NULL;
  rawindex.used      = 0;
  rawindex.allocated = 0;

  bzero(&request_data, sizeof(struct grst_request));
  request_data.writefunction = rawbody_callback;
  request_data.writedata     = (void *) &rawindex;
  request_data.errorbuffer   = errorbuffer;
  request_data.url           = url;
  request_data.method        = GRST_SLASH_GET;
  request_data.start         = -1;
  request_data.finish        = -1;
  
  if (debugmode) syslog(LOG_DEBUG, "Get directory listing from URL %s", url);
  
  thiserror = perform_request(&request_data, &fuse_ctx);

  if ((thiserror != 0) ||
           (request_data.retcode <  200) ||
           (request_data.retcode >= 300))
         {
           if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);
           
           if (thiserror != 0) anyerror = thiserror;
           else                anyerror = request_data.retcode;

           if (request_data.retcode == 403) return -EACCES;
           else return -ENOENT; 
/* memory clean up still needed here!!!!!! */
         }
  else
         {
           time(&now);

           filler(buf, ".", NULL, 0);
           filler(buf, "..", NULL, 0);

           rawindex.text[rawindex.used] = '\0';

           list  = index_to_dir_list(rawindex.text, url);
           ilast = -1;

           for (i=0; list[i].filename != NULL; ++i)
              {
                if (strncmp(list[i].filename, "mailto:", 7) == 0) continue;

                /* skip over duplicates */
                
                if ((ilast >= 0) && 
                    (strcmp(list[i].filename, list[ilast].filename) == 0))
                                                                 continue;
                ilast=i; /* last distinct entry */
                
                asprintf(&s, "%s/%s", path, list[i].filename);
                write_headers_to_cache(&fuse_ctx, s, list[i].length, 
                                       list[i].modified);
                free(s);
           
                bzero(&stat_tmp, sizeof(struct stat));
                stat_tmp.st_size  = list[i].length;
                stat_tmp.st_mtime = list[i].modified;
                stat_tmp.st_ctime = list[i].modified;
                stat_tmp.st_atime = now;
                stat_tmp.st_mode  = 0666;
            
                filler(buf, list[i].filename, &stat_tmp, 0);
              }
         }
     
  return 0;
}

static int slashgrid_getattr(const char *rawpath, struct stat *stbuf)
{
  int          anyerror = 0, thiserror, i, ilast, len;
  char        *s, *url, *path, errorbuffer[CURL_ERROR_SIZE+1] = "";
  struct       grst_dir_list   *list;
  struct       grst_request request_data;
  struct       tm               modified_tm;
  struct       stat             stat_tmp;
  time_t                        now;

  struct fuse_context fuse_ctx;

  memcpy(&fuse_ctx, fuse_get_context(), sizeof(struct fuse_context));

  if (debugmode) syslog(LOG_DEBUG, 
                         "in slashgrid_getattr, rawpath=%s, UID=%d\n",
                         rawpath, fuse_ctx.uid);

  memset(stbuf, 0, sizeof(struct stat));
  stbuf->st_mode  = S_IFREG | 0755;
  stbuf->st_nlink = 1;
  
  if ((strcmp(rawpath, "/")      == 0) ||
      (strcmp(rawpath, "/http")  == 0) ||
      (strcmp(rawpath, "/https") == 0))
    {
      stbuf->st_mode = S_IFDIR | 0755;
              
      return 0; /* The empty top level directory: OK */
    }
  else if (strncmp(rawpath, "/http/", 6) == 0)
    {
      if (index(&rawpath[6], '/') == NULL) /* top directory for remote server */
        {
          stbuf->st_mode = S_IFDIR | 0755;
          
          asprintf(&url, "http://%s/", &rawpath[6]);
          asprintf(&path, "%s/", rawpath);
        }
      else 
        {
          asprintf(&url, "http://%s", &rawpath[6]);      
          path = strdup(rawpath);
        }
    }
  else if (strncmp(rawpath, "/https/", 7) == 0)
    {
      if (index(&rawpath[7], '/') == NULL) /* top directory for remote server */
        {
          stbuf->st_mode = S_IFDIR | 0755;

          asprintf(&url, "https://%s/", &rawpath[7]);
          asprintf(&path, "%s/", rawpath);
        }
      else 
        {
          asprintf(&url, "https://%s", &rawpath[7]);
          path = strdup(rawpath);
        }
    }
  else return -ENOENT;
  
  time(&now);

  if (read_headers_from_cache(&fuse_ctx, path, 
                              &(stbuf->st_size), &(stbuf->st_mtime)))
    {
      if (debugmode) syslog(LOG_DEBUG, 
          "Retrieving details for %s from cache (%ld %ld)\n", url,
          (long) stbuf->st_mtime, (long) stbuf->st_size);
    
      stbuf->st_ctime = stbuf->st_mtime;
      stbuf->st_atime = now;
      
      free(url);
      free(path);
      return 0;    
    }

  bzero(&request_data, sizeof(struct grst_request));
  request_data.writefunction = null_callback;
  request_data.writedata     = NULL;
  request_data.errorbuffer   = errorbuffer;
  request_data.url           = url;
  request_data.method        = GRST_SLASH_HEAD;
  request_data.start         = -1;
  request_data.finish        = -1;

  thiserror = perform_request(&request_data, &fuse_ctx);

  if ((thiserror != 0) ||
           (request_data.retcode < 200) ||
           (request_data.retcode > 301))
         {
           if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);
           
           if (thiserror != 0) anyerror = thiserror;
           else                anyerror = request_data.retcode;

           free(url);
           free(path);
           
           if (request_data.retcode == 403) return -EACCES;
           else return -ENOENT; 
/* memory clean up still needed here!!!!!! */
         }
         
  if (request_data.retcode == 301)
    {
       if (debugmode) syslog(LOG_DEBUG, "301 detected");

       len = strlen(url);
      
       if ((request_data.location != NULL) &&
          (len + 1 == strlen(request_data.location)) &&
          (request_data.location[len] == '/') &&
          (strncmp(url, request_data.location, len) == 0))
        {
          request_data.length_set   = 0;
          request_data.modified_set = 0;
          request_data.retcode      = 0;
          request_data.url          = request_data.location;
        
          thiserror = perform_request(&request_data, &fuse_ctx);

          if ((thiserror != 0) ||
              (request_data.retcode < 200) ||
              ((request_data.retcode > 299) && (request_data.retcode != 403)))
            {
              if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);
           
              if (thiserror != 0) anyerror = thiserror;
              else                anyerror = request_data.retcode;

              free(url);
              free(path);
              return -ENOENT; 
/* memory clean up still needed here!!!!!! */
            }
            
          stbuf->st_mode  = S_IFDIR | 0755;  /* this is a directory */

          free(path);
          asprintf(&path, "%s/", rawpath);
        }
      else 
        {
          free(url);
          free(path);
          return -ENOENT;
        }
    }

  if (request_data.length_set) stbuf->st_size  = request_data.length;
  else stbuf->st_size = 0;
  
  if (request_data.modified_set)
    {
      stbuf->st_mtime = request_data.modified;
      stbuf->st_ctime = request_data.modified;
    }

  stbuf->st_atime = now;

  write_headers_to_cache(&fuse_ctx, path, stbuf->st_size, stbuf->st_mtime);

  free(url);
  free(path);
  return 0;
}


int write_block_to_cache(struct fuse_context *fuse_ctx, char *filename,  
                         off_t start, off_t finish)
{
  int          anyerror = 0, thiserror, i, fd;
  char        *s, *url, *tempfile, *encoded_filename, *p,
              *newdir, *new_filename, errorbuffer[CURL_ERROR_SIZE+1] = "";
  struct       stat statbuf;
  struct       grst_request request_data;
  FILE        *fp;

  asprintf(&tempfile, "%s/blocks-XXXXXX", GRST_SLASH_TMP);
  fd = mkstemp(tempfile);

  if (fd == -1)
    {
      free(tempfile);
      return -EIO;
    }

  fp = fdopen(fd, "w");

  if (strncmp(filename, "/http/", 6) == 0)
    asprintf(&url, "http://%s", &filename[6]);
  else if (strncmp(filename, "/https/", 7) == 0)
    asprintf(&url, "https://%s", &filename[7]);
  else return -ENOENT;

  bzero(&request_data, sizeof(struct grst_request));
  request_data.writefunction = fwrite;
  request_data.writedata     = (void *) fp;
  request_data.errorbuffer   = errorbuffer;
  request_data.url           = url;
  request_data.method        = GRST_SLASH_GET;
  request_data.start         = start;
  request_data.finish        = finish;

  if (debugmode) syslog(LOG_DEBUG, "Get block %ld-%ld from URL %s\n",
                                   (long) start, (long) finish, url);
  
  thiserror = perform_request(&request_data, fuse_ctx);

  free(url);

  fclose(fp);  

  if ((thiserror != 0) ||
           (request_data.retcode <  200) ||
           (request_data.retcode >= 300))
         {
           if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);

           if (thiserror != 0) anyerror = thiserror;
           else                anyerror = request_data.retcode;

           if (request_data.retcode == 403) return -EACCES;
           else return -ENOENT; 
/* memory clean up still needed here!!!!!! */
         }

  encoded_filename = GRSThttpUrlMildencode(filename);

// need to protect against .. ?
// can optimise by checking for existing of filename as a dir at the start

  for (p = encoded_filename; ; ++p)
     {  
       if ((*p != '/') && (*p != '\0')) continue;
     
       if (*p == '/') 
         {
           *p = '\0';
           asprintf(&newdir, "%s/%d%s", 
                    GRST_SLASH_BLOCKS, fuse_ctx->uid, encoded_filename);
           *p = '/';
         }
       else asprintf(&newdir, "%s/%d%s", 
                     GRST_SLASH_BLOCKS, fuse_ctx->uid, encoded_filename);
           
       if (stat(newdir, &statbuf) != 0)
                                   mkdir(newdir, S_IRUSR | S_IWUSR | S_IXUSR);
       free(newdir);
       
       if (*p == '\0') break;
     }

  asprintf(&new_filename, "%s/%d%s/%ld-%ld", 
       GRST_SLASH_BLOCKS, fuse_ctx->uid, encoded_filename, (long) start, (long) finish);

  free(encoded_filename);
  
  rename(tempfile, new_filename);

  if (debugmode) syslog(LOG_DEBUG, "Added %s to block cache", new_filename);

  free(tempfile);
  free(new_filename);

  return 0;
}

int drop_cache_blocks(struct fuse_context *fuse_ctx, char *filename)
/* drop ALL the blocks cached for this file, and delete the directory in
   the blocks cache for this file */
{
  int   ret;
  char *encoded_filename, *dirname, *blockname;
  DIR *blocksDIR;
  struct dirent *blocks_ent;

  encoded_filename = GRSThttpUrlMildencode(filename);
  
  asprintf(&dirname, "%s/%d%s", 
                     GRST_SLASH_BLOCKS, fuse_ctx->uid, encoded_filename);

  free(encoded_filename);

  blocksDIR = opendir(dirname);
  
  if (blocksDIR == NULL) /* no directory to delete (probably) */
    {
      free(dirname);
      return 1;
    }
    
  while ((blocks_ent = readdir(blocksDIR)) != NULL)
       {
         asprintf(&blockname, "%s/%s", dirname, blocks_ent->d_name);
         remove(blockname);
         free(blockname);
       }  
  
  closedir(blocksDIR);
    
  ret = rmdir(dirname);
  free(dirname);  

  return ret ? 1 : 0; /* return 1 on error, 0 on rmdir() success */
}

static int slashgrid_read(const char *path, char *buf, 
                          size_t size, off_t offset,
                          struct fuse_file_info *fi)
{
  (void) offset;
  (void) fi;

  int          anyerror = 0, thiserror, i, ilast, fd;
  char        *s, *url, *disk_filename, *encoded_filename;
  off_t        block_start, block_finish, block_i, len;
  struct       grst_body_text   rawbody;
  struct       grst_request request_data;
  struct       tm               modified_tm;
  struct       stat             statbuf;
  time_t                        now;
  struct fuse_context fuse_ctx;

  memcpy(&fuse_ctx, fuse_get_context(), sizeof(struct fuse_context));
  
  if (debugmode) syslog(LOG_DEBUG, "in slashgrid_read size=%ld offset=%ld",
                                    (long) size, (long) offset);

  if ((strncmp(path, "/http/",  6) != 0) &&
      (strncmp(path, "/https/", 7) != 0)) return -ENOENT;

  block_start  = GRST_SLASH_BLOCK_SIZE * (offset / GRST_SLASH_BLOCK_SIZE);
  block_finish = GRST_SLASH_BLOCK_SIZE *
                                ((offset + size - 1) / GRST_SLASH_BLOCK_SIZE);

  encoded_filename = GRSThttpUrlMildencode((char *) path);
  time(&now);
 
  for (block_i = block_start; block_i <= block_finish; block_i += GRST_SLASH_BLOCK_SIZE)
     {     
       asprintf(&disk_filename, "%s/%d%s/%ld-%ld", 
                 GRST_SLASH_BLOCKS, fuse_ctx.uid, encoded_filename, 
                 (long) block_i, (long) (block_i + GRST_SLASH_BLOCK_SIZE - 1));

       if (debugmode) syslog(LOG_DEBUG, "disk_filename=%s", disk_filename);
                 
       if ((stat(disk_filename, &statbuf) != 0) ||
           (statbuf.st_mtime < now - GRST_SLASH_HEADERS_EXPIRE))
         {
           write_block_to_cache(&fuse_ctx, (char *) path, 
                            block_i, block_i + GRST_SLASH_BLOCK_SIZE - 1);
         }

// need to worry about cached copy being deleted (invalidated by a writing
// thread?) between write_block_to_cache() and these reads?
// maybe return fd from write_block_to_cache() itself???
// the initial stat() needs to be part of this too

       if ((fd = open(disk_filename, O_RDONLY)) != -1)
         {
           if (block_i == block_start)              
             {
               lseek(fd, offset - block_start, SEEK_SET);
               read(fd, buf, 
                        (offset - block_start + size < GRST_SLASH_BLOCK_SIZE) 
                       ? size : GRST_SLASH_BLOCK_SIZE - offset + block_start);
             }
           else if (block_i == block_finish)
             {
               read(fd, buf + (block_i - block_start),
                        offset + size - block_i);
             }
           else 
             {
               read(fd, buf + (block_i - block_start), 
                        GRST_SLASH_BLOCK_SIZE);
             }
             
           close(fd);
         }        
       else syslog(LOG_ERR, "Failed to open %s in cache", disk_filename);
     }

  free(disk_filename);
  free(encoded_filename);

  return size;
}

static int slashgrid_write(const char *path, const char *buf, 
                           size_t size, off_t offset,
                           struct fuse_file_info *fi)
{
  int          anyerror = 0, thiserror, i, fd;
  char        *s, *url, *p, errorbuffer[CURL_ERROR_SIZE+1] = "";

  struct grst_read_data read_data;
  struct grst_request request_data;
  struct fuse_context fuse_ctx;
  
  memcpy(&fuse_ctx, fuse_get_context(), sizeof(struct fuse_context));  

  if (strncmp(path, "/http/", 6) == 0)
    asprintf(&url, "http://%s", &path[6]);
  else if (strncmp(path, "/https/", 7) == 0)
    asprintf(&url, "https://%s", &path[7]);
  else return -ENOENT;

  read_data.buf     = buf;
  read_data.sent    = 0;
  read_data.maxsent = size;

  if (debugmode) syslog(LOG_DEBUG, "Put block %ld-%ld to URL %s", 
                                   (long) offset, (long) offset+size-1, url);

  drop_cache_blocks(&fuse_ctx, (char *) path); /* we drop all read-cache blocks first */
  
  bzero(&request_data, sizeof(struct grst_request));
  request_data.writefunction = null_callback;
  request_data.readfunction  = read_data_callback;
  request_data.readdata      = &read_data;
  request_data.infilesize    = size;
  request_data.errorbuffer   = errorbuffer;
  request_data.url           = url;
  request_data.method        = GRST_SLASH_PUT;
  request_data.start         = offset;
  request_data.finish        = (off_t) (offset + size - 1);

  thiserror = perform_request(&request_data, &fuse_ctx);

  free(url);

  if ((thiserror != 0) ||
      (request_data.retcode <  200) ||
      (request_data.retcode >= 300))
    {
           if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);
           
           if (thiserror != 0) anyerror = thiserror;
           else                anyerror = request_data.retcode;

           if (request_data.retcode == 403) return -EACCES;
           else return -ENOENT; 
/* memory clean up still needed here!!!!!! */
    }

  return size;
}

int slashgrid_rename(const char *oldpath, const char *newpath)
{
  int          anyerror = 0, thiserror, i, fd;
  char        *s, *url, *p, *destination, errorbuffer[CURL_ERROR_SIZE+1] = "";

  struct grst_read_data read_data;
  struct fuse_context fuse_ctx;
  struct grst_request request_data;

  memcpy(&fuse_ctx, fuse_get_context(), sizeof(struct fuse_context));

  if (strncmp(oldpath, "/http/", 6) == 0)
    {
      if (strncmp(newpath, "/http/", 6) != 0) return -EXDEV;

      asprintf(&url,         "http://%s", &oldpath[6]);
      asprintf(&destination, "http://%s", &newpath[6]);
    }
  else if (strncmp(oldpath, "/https/", 7) == 0)
    {
      if (strncmp(newpath, "/https/", 7) != 0) return -EXDEV;

      asprintf(&url,         "https://%s", &oldpath[7]);
      asprintf(&destination, "https://%s", &newpath[7]);
    }
  else return -ENOENT;

  read_data.buf     = "";
  read_data.sent    = 0;
  read_data.maxsent = 0;

  if (debugmode) syslog(LOG_DEBUG, "MOVE URL %s to %s", url, destination);
  
  bzero(&request_data, sizeof(struct grst_request));
  request_data.writefunction = null_callback;
  request_data.readfunction  = read_data_callback;
  request_data.readdata      = &read_data;
  request_data.errorbuffer   = errorbuffer;
  request_data.url           = url;
  request_data.method        = GRST_SLASH_MOVE;
  request_data.destination   = destination;

  thiserror = perform_request(&request_data, &fuse_ctx);

  free(url);
  free(destination);

  if ((thiserror != 0) ||
      (request_data.retcode <  200) ||
      (request_data.retcode >= 300))
    {
      if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);

      if (thiserror != 0) anyerror = thiserror;
      else                anyerror = request_data.retcode;

      if (request_data.retcode == 403) return -EACCES;
      else return -ENOENT; 
/* memory clean up still needed here!!!!!! */
    }

  return 0;
}

int slashgrid_unlink(const char *path)
{
  int          anyerror = 0, thiserror, i, fd;
  char        *s, *url, *p, errorbuffer[CURL_ERROR_SIZE+1] = "";

  struct grst_read_data read_data;
  struct fuse_context fuse_ctx;
  struct grst_request request_data;

  memcpy(&fuse_ctx, fuse_get_context(), sizeof(struct fuse_context));

  if (strncmp(path, "/http/", 6) == 0)
    asprintf(&url, "http://%s", &path[6]);
  else if (strncmp(path, "/https/", 7) == 0)
    asprintf(&url, "https://%s", &path[7]);
  else return -ENOENT;

  read_data.buf     = "";
  read_data.sent    = 0;
  read_data.maxsent = 0;

  if (debugmode) syslog(LOG_DEBUG, "DELETE URL %s", url);
  
  bzero(&request_data, sizeof(struct grst_request));
  request_data.writefunction = null_callback;
  request_data.readfunction  = read_data_callback;
  request_data.readdata      = &read_data;
  request_data.errorbuffer   = errorbuffer;
  request_data.url           = url;
  request_data.method        = GRST_SLASH_DELETE;

  thiserror = perform_request(&request_data, &fuse_ctx);

  free(url);

  if ((thiserror != 0) ||
           (request_data.retcode <  200) ||
           (request_data.retcode >= 300))
         {
           if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);

           if (thiserror != 0) anyerror = thiserror;
           else                anyerror = request_data.retcode;

           if (request_data.retcode == 403) return -EACCES;
           else return -ENOENT; 
/* memory clean up still needed here!!!!!! */
         }

  return 0;
}

int slashgrid_rmdir(const char *path)
{
  int   ret;
  char *pathwithslash;
  
  asprintf(&pathwithslash, "%s/", path);
  ret = slashgrid_unlink(pathwithslash);  
  free(pathwithslash);

/* error on GridSite side still??? */
  
  return ret;
}

int slashgrid_mknod(const char *path, mode_t mode, dev_t dev)
{
  int ret;
  
  ret = slashgrid_write(path, "", 0, 0, NULL);

  return (ret < 0) ? ret : 0;
}

int slashgrid_mkdir(const char *path, mode_t mode)
{
  int   ret;
  char *pathwithslash;
  
  asprintf(&pathwithslash, "%s/", path);
  ret = slashgrid_write(pathwithslash, "", 0, 0, NULL);
  free(pathwithslash);

  return (ret < 0) ? ret : 0;
}

int slashgrid_chown(const char *path, uid_t uid, gid_t gid)
{
  puts("slashgrid_chown - NOP");
  return 0;
}

int slashgrid_chmod(const char *path, mode_t mode)
{
  puts("slashgrid_chmod - NOP");
  return 0;
}

int slashgrid_truncate(const char *path, off_t offset)
{
  int          anyerror = 0, thiserror, i, fd;
  char        *s, *url, *p, errorbuffer[CURL_ERROR_SIZE+1] = "";

  struct grst_read_data read_data;
  struct fuse_context fuse_ctx;
  struct grst_request request_data;

  memcpy(&fuse_ctx, fuse_get_context(), sizeof(struct fuse_context));

  if (strncmp(path, "/http/", 6) == 0)
    asprintf(&url, "http://%s", &path[6]);
  else if (strncmp(path, "/https/", 7) == 0)
    asprintf(&url, "https://%s", &path[7]);
  else return -ENOENT;

  read_data.buf     = "";
  read_data.sent    = 0;
  read_data.maxsent = 0;

  if (debugmode) syslog(LOG_DEBUG, "Truncate URL %s to %ld\n", 
                                   url, (long) offset);
  
  bzero(&request_data, sizeof(struct grst_request));
  request_data.writefunction = null_callback;
  request_data.readfunction  = read_data_callback;
  request_data.readdata      = &read_data;
  request_data.errorbuffer   = errorbuffer;
  request_data.url           = url;
  request_data.method        = GRST_SLASH_TRUNC;
  request_data.finish        = offset;

  thiserror = perform_request(&request_data, &fuse_ctx);

  free(url);

  if ((thiserror != 0) ||
           (request_data.retcode <  200) ||
           (request_data.retcode >= 300))
         {
           if (debugmode)
                syslog(LOG_DEBUG, "... curl error: %s (%d), HTTP error: %d\n",
                       errorbuffer, thiserror, request_data.retcode);

           if (thiserror != 0) anyerror = thiserror;
           else                anyerror = request_data.retcode;

           if (request_data.retcode == 403) return -EACCES;
           else return -ENOENT; 
/* memory clean up still needed here!!!!!! */
         }

  return 0;
}

int slashgrid_statfs(const char *path, struct statfs *fs)
{
  return statfs(GRST_SLASH_BLOCKS, fs);
}

void *slashgrid_init(void)
{
  FILE *fp;
  
  if ((fp = fopen(GRST_SLASH_PIDFILE, "w")) != NULL)
    {
      fprintf(fp, "%d\n", (int) getpid());
      fclose(fp);
    }

  return NULL;
}

void slashgrid_destroy(void *p)
{
  unlink(GRST_SLASH_PIDFILE);
}

static struct fuse_operations slashgrid_oper = {
  .getattr	= slashgrid_getattr,
  .chown	= slashgrid_chown,    
  .chmod	= slashgrid_chmod,
  .truncate	= slashgrid_truncate,    
  .readdir	= slashgrid_readdir,
  .write	= slashgrid_write,
  .read		= slashgrid_read,
  .mknod	= slashgrid_mknod,
  .mkdir	= slashgrid_mkdir,
  .unlink	= slashgrid_unlink,
  .rmdir	= slashgrid_rmdir,
  .rename	= slashgrid_rename,
  .statfs	= slashgrid_statfs,
  .init		= slashgrid_init,
  .destroy	= slashgrid_destroy
};

int main(int argc, char *argv[])
{
#define FUSE_ARGC 4
  char *fuse_argv[FUSE_ARGC] = { "slashgrid", "/grid", "-o", "allow_other" };
  int   i, ret;
  
  for (i=1; i < argc; ++i)
    if (strcmp(argv[i], "--debug") == 0) debugmode = 1;

  openlog("slashgrid", 0, LOG_DAEMON);
    
  umount("/grid"); /* in case of a crash, but will fail if still busy */

  for (i=0; i < GRST_SLASH_MAX_HANDLES; ++i)
     {
       pthread_mutex_init(&(handles[i].mutex), NULL);
       handles[i].curl_handle = NULL;
       handles[i].proxyfile   = NULL;
       handles[i].last_used   = 0;
     }

  ret = fuse_main(FUSE_ARGC, fuse_argv, &slashgrid_oper);

  return ret;
}
