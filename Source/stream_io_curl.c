#include "global.h"
#include "stream.h"
#include "debug.h"
#include "file.h"
#include "util.h"
#include "astdlib.h"
#include "cb.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include <curl/curl.h>

#ifdef CONFIG_STREAM_IO_CURL

#define DBGERR 	if( 1 )
#define DBGS 	if( Debug[DBG_STREAM] )
#define DBG    	if( Debug[DBG_HD]     )

static int use_select = 1;
static int dbg_curl   = 0;
static int curl_fail = 0;
static int curl_gotnot = 0;

#define DBG2   	if( dbg_curl == 1 )
#define DBGC   	if( dbg_curl == 2 )

#ifdef DEBUG_MSG

static void _dbg_curl( int argc, UCHAR **argv ) 
{ 
	if( argc < 2 ) {
		dbg_curl = dbg_curl ? 0 : 1;
	} else {
		dbg_curl = atoi( argv[1] );
	}
serprintf("CURL: dbg_curl %d\r\n", dbg_curl ); 
}

DECLARE_DEBUG_COMMAND_VOID("dbgcu",   _dbg_curl );
DECLARE_DEBUG_TOGGLE      ("curls",  use_select );
DECLARE_DEBUG_TOGGLE      ("curlf",  curl_fail );
DECLARE_DEBUG_TOGGLE      ("curlgn", curl_gotnot );
#endif

#define MAX_URL_LEN 2048

typedef struct s_CURL_PRIV {
	char 		url[MAX_URL_LEN + 1];
	CIRCULAR_BUFFER cb;
	CURL  		*curl;
	CURLM 		*multi_handle;
	struct curl_slist *headers;
	unsigned char 	*read_buffer;
	int 		read_count;
	int 		total;
	int		got_header;
	UINT64		size;
	int		redirect;
	int		seekable;
	int		done;
	
	int		ytgv;		// Youtube/GoogleVideo
	int		ignore_size;
} CURL_PRIV;

static size_t curl_write( void *_ptr, size_t size, size_t nmemb, void *stream)
{
	UCHAR *ptr = _ptr;
	STREAM_IO *io = stream;
	CURL_PRIV *priv = io->priv;
	int count = size * nmemb;
	
	if( priv->ignore_size ) {
		// we need to ignore bytes
		int ignore = MIN( priv->ignore_size, count );
serprintf("curl ignore: %d\r\n", ignore);
		priv->ignore_size -= ignore;
		count             -= ignore;
		ptr               += ignore;		
	}
	int copy = MIN( count, priv->read_count );
	
DBGC serprintf("W%6d", count );	
DBG2 serprintf("write: count %8d  read count %d\r\n", count, priv->read_count );
	priv->total += count;

	if( copy ) {
		memcpy( priv->read_buffer, ptr, copy );
		
		priv->read_buffer += copy;
		priv->read_count  -= copy;
		
		ptr   += copy;
		count -= copy;
	}	

	if( count ) {
		//copy rest to buffer
DBG2 serprintf("copy to circ: %d\r\n", count );
		cb_write( &priv->cb, ptr, count );
	}
		
	priv->got_header = 1;
	return size * nmemb;
}

static size_t _write_header( void *_ptr, size_t size, size_t nmemb, void *stream)
{
	STREAM_IO *io = stream;
	CURL_PRIV *priv = io->priv;
	int count = size * nmemb;

DBG serprintf("\t");
	UCHAR *c = _ptr;
	while( count-- ) {
DBG serprintf("%c", *(c++));	
	}		

	if( io->meta ) {
		io->meta( io->meta_ctx, _ptr, size * nmemb );
	}
	
	if( !priv->got_header ) {
		UCHAR *ptr = _ptr;
		if( !strncasecmp( ptr, "Content-Length:", strlen("Content-Length:") ) ) {
			UINT64 size;
			sscanf( ptr + strlen("Content-Length:") + 1, "%"PRIu64, &size );
			if( priv->redirect ) {
DBG serprintf("IGNORE size: %lld\r\n", size );		
			} else {
DBG serprintf("size: %lld\r\n", size );		
				priv->size = size;
			}
		} else if ( !strncasecmp( ptr, "Accept-Ranges: bytes", strlen( "Accept-Ranges: bytes") ) ) {
DBG serprintf("seekable!\r\n" );		
			priv->seekable = 1;
		} else if ( !strncasecmp( ptr, "Location:", strlen( "Location:") ) ) {
DBG serprintf("redirect!\r\n" );	
			priv->redirect = 1;
			priv->size = 0;
		} else if ( !strncasecmp( ptr, "HTTP/", strlen( "HTTP/") ) ) {
DBG serprintf("no redirect!\r\n" );		
			priv->redirect = 0;
		} else if ( !strncasecmp( ptr, "Server:", strlen( "Server:") ) ) {
			if( strstrNC( ptr, "serviio" ) ) {
DBG serprintf("serviio, assume seekable!\r\n" );		
				priv->seekable = 1;
			} else if( strstrNC( ptr, "MiniWebServer" ) ) {
DBG serprintf("MiniWebServer (FreeMi), assume seekable!\r\n" );		
				priv->seekable = 1;
			}
		}
	}
	
	return size * nmemb;
}

static int calc_timeout(char *url, int *connect, int *transfer)
{
	int local = 0;

	if (strstr(url, "http://192.168.") != NULL)
		local = 1;
	else if (strstr(url, "http://10.") != NULL)
		local = 1;
	else if (strstr(url, "http://172.16.") != NULL) // this is not perfect but should be ok
		local = 1;
	else
		local = 0;
		
	if (local) {
		*connect  = 5;
		*transfer = 0;
	} else {
		*connect  = 20;
		*transfer = 0;
	}
	
DBG serprintf("%s: curl is %s, timeouts are connection %d transfer %d\n", __FUNCTION__, (local) ? "LOCAL" : "REMOTE", *connect, *transfer);
	
	return 1;
}

static int _isHTTPCodeOk( long http_code ) {

	return ( http_code == 200 || http_code == 206 );
}

static int _check_msgs( CURL_PRIV *priv, int *done, int *retry )
{
	*done  = 0;
	*retry = 0;
	
	int msgs_in_queue = 0;
	int max = 0;
	CURLMsg *msg = curl_multi_info_read(priv->multi_handle, &msgs_in_queue);
	// Get messages to see if a download is finished
	if( curl_gotnot ) {
		curl_gotnot = 0;
serprintf("CURL: forced error\n");
		*retry = 1;
		return 1;				
	}
	while (msg != NULL && max++ < 10 ) {
		char *info_url;
		int ret = curl_easy_getinfo(msg->easy_handle, CURLINFO_EFFECTIVE_URL, &info_url);
		if (ret != CURLE_OK) {
serprintf("\r\ncurl_easy_getinfo failed (url): %d\r\n", ret);
		} else {
			if ( msg->data.result == CURLE_OK ) {
				long http_code = 0;

				ret = curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);

				if ( ret == CURLE_OK && _isHTTPCodeOk( http_code ) ) {
DBG serprintf("\r\nTransfer for URL %s completed\r\n", info_url);
					*done = 1;
					return 0;				
				}  else {
serprintf("\r\n[%s] Transfer for URL %s failed ! HTTP code : %d\r\n", __FUNCTION__, info_url, (int)http_code);
					return 1;
				}
			} else if(  msg->data.result == CURLE_GOT_NOTHING ) {
serprintf("\r\n[%s] Transfer for URL %s: CURLE_GOT_NOTHING\n", __FUNCTION__, info_url );
				*retry = 1;
				return 1;
			} else if (msg->data.result == CURLE_PARTIAL_FILE) {
serprintf("\r\n[%s] Transfer for URL %s: CURLE_PARTIAL_FILE\n", __FUNCTION__, info_url );
				*retry = 1;
				return 1;
			} else {
serprintf("\r\n[%s] Transfer for URL %s failed ! Error code : %d\r\n", __FUNCTION__, info_url, msg->data.result);
				return 1;
			}
		}
		msg = curl_multi_info_read( priv->multi_handle, &msgs_in_queue);
	}
	return 0;
}

static int curl_open( STREAM_IO *io, UINT64 offset )
{
	int connect_timeout;
	int transfer_timeout;
	CURL_PRIV *priv = io->priv;

DBG serprintf("curl_open at %lld\r\n", offset );
	if( !(priv->curl = curl_easy_init())  ) {
serprintf("_curl_easy_init failed\r\n");
		return 1;	
	}
	
	char *url = priv->url;
	
	// for YT and GV, we need to set the &start=xyz value in the URL
	char new_url[MAX_URL_LEN + 20 + 1];
	if( offset && priv->ytgv ) {
		strcpy( new_url, priv->url );
		// find the "origin=" string
		char *old_origin = strstr( priv->url, "origin=" );
		char *new_origin = strstr( new_url,   "origin=" );
		if ( old_origin && new_origin ) {
			// add the "&start="
			sprintf( new_origin, "start=%"PRIu64"&", offset );
			// and the rest of the old URL
			strcat( new_url, old_origin );
serprintf("curl: new url: [%s]\r\n", new_url );
			url = new_url;		
			priv->ignore_size = 13;
			offset = 0;
		}
	}
	
	curl_easy_setopt(priv->curl, CURLOPT_URL,            url );
	curl_easy_setopt(priv->curl, CURLOPT_FOLLOWLOCATION, 1 );
	curl_easy_setopt(priv->curl, CURLOPT_MAXREDIRS,      -1 );
	curl_easy_setopt(priv->curl, CURLOPT_SSL_VERIFYPEER, FALSE);
DBG	curl_easy_setopt(priv->curl, CURLOPT_VERBOSE,        1 );
DBGC	curl_easy_setopt(priv->curl, CURLOPT_VERBOSE,        1 );

	priv->headers = curl_slist_append( priv->headers, "User-Agent: curl/"LIBCURL_VERSION" avos (Linux;Android)");

	if( io->src.extra_list ) {
		int i = 0;
		while( io->src.extra_list[i] ) {
			priv->headers = curl_slist_append( priv->headers, io->src.extra_list[i]);
			++i;
		}
	}
	if( io->setup ) {
		// some users might need to provide some HTTP headers, so we call the callback
		io->setup( io->setup_ctx, (void*)(&priv->headers) );
	}

//	if( network_get_proxy()){
//		curl_easy_setopt(priv->curl, CURLOPT_PROXY, network_get_proxy() );
//	}

	curl_easy_setopt(priv->curl, CURLOPT_HTTPHEADER, priv->headers );

	curl_easy_setopt(priv->curl, CURLOPT_WRITEFUNCTION,  curl_write);
 	curl_easy_setopt(priv->curl, CURLOPT_WRITEDATA,      (void*)io);
	curl_easy_setopt(priv->curl, CURLOPT_HEADERFUNCTION, _write_header);
	curl_easy_setopt(priv->curl, CURLOPT_WRITEHEADER,    (void*)io);
	
	curl_easy_setopt(priv->curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);
	
	if (calc_timeout(priv->url, &connect_timeout, &transfer_timeout)) {
		if (connect_timeout || transfer_timeout)
			curl_easy_setopt(priv->curl, CURLOPT_NOSIGNAL, 1);
		if (connect_timeout)
			curl_easy_setopt(priv->curl, CURLOPT_CONNECTTIMEOUT, connect_timeout);
		if (transfer_timeout)
			curl_easy_setopt(priv->curl, CURLOPT_TIMEOUT, transfer_timeout);
	}
 
	priv->multi_handle = curl_multi_init();
	curl_multi_add_handle(priv->multi_handle, priv->curl);

	// we want the header here, so we "perform" 
	// until the first _curl_write call
	if( !priv->got_header ) {
		int running = 0;
		do {
			curl_multi_perform(priv->multi_handle, &running);

			int done;
			int retry;
			if( _check_msgs( priv, &done, &retry ) ) {
				// something is wrong, abort!
				return 1;
			}
			msec_sleep( 1 );
		}
		while( !priv->got_header && running );
DBG serprintf("curl: got header\r\n" );
	}
	
	return 0;
}

static int curl_close( CURL_PRIV *priv )
{
   	curl_multi_remove_handle(priv->multi_handle, priv->curl);
	curl_easy_cleanup(priv->curl);
	curl_slist_free_all( priv->headers );
	priv->headers = NULL;
	curl_multi_cleanup(priv->multi_handle);
	return 0;
}

static int _open( STREAM_IO *io, int mode )
{
	CURL_PRIV *priv = io->priv;
	
DBGS serprintf("stream_io_open_CURL: %s\r\n", io->src.url);

	strnZcpy( priv->url, io->src.url, MAX_URL_LEN );
		
	priv->total = 0;
	io->pos     = 0;
	if ( cb_malloc( &priv->cb, CURL_MAX_WRITE_SIZE + 1 ) ) {
		return 1;
	}

	priv->got_header = 0;
	curl_open( io, 0 );
	
	if( priv->got_header ) {
		// we have the size and seekable data, update the stream with it!
		if( priv->size ) {
			io->size = priv->size;
		} else {
			// just put the largest value!
			io->size = (UINT64)0xFFFFFFFFFFFFFFFull;		
serprintf("curl: no size from server, use %llX\r\n", io->size);
			// and do not allow to seek!
			priv->seekable = 0;
		}
	}

	// youtube and googlevideo do not support "bytes=" any more, so we have to do a special seek for them:
	if ( strcasestr( io->src.url, "get_video" ) && (strcasestr( io->src.url, "googlevideo" ) || strcasestr( io->src.url, "youtube" ) ) ) {
serprintf("youtube or googlevideo, special seeking needed!\r\n");
		priv->ytgv     = 1;
	} 
	io->_is_open = 1;
	return 0;
}

static int _is_open( STREAM_IO *io )
{
	return io ? io->_is_open : 0;
}

static int _close( STREAM_IO *io )
{
	CURL_PRIV *priv = io->priv;

DBGS serprintf("stream_io_close_CURL\r\n");
	if( !io->_is_open ) {
serprintf("not open!\r\n");
		return 1;
	}
  	curl_close( priv );
	
	cb_free( &priv->cb );

	// free priv data
	afree( io->priv );
	io->priv = NULL;
	
	io->_is_open = 0;
	return 0;
}

static UINT64 _seek( STREAM_IO *io, UINT64 pos)
{
	CURL_PRIV *priv = io->priv;
	UINT64 ret = pos;

	if( pos != io->pos ) {
DBG serprintf("curl_seek( %lld/%lld )", pos, io->pos );
		curl_close( priv);
		cb_flush( &priv->cb );
		curl_open( io, pos );
		ret = pos;
DBGERR {
if( ret != pos )
	serprintf("stream_io_curl_seek ERR pos %d  _pos %d \r\n", pos, io->pos );	
}

		priv->done = 0;
		io->pos    = ret;
	}

	return ret;
}

static int _try_read( STREAM_IO *io, UCHAR *buffer, UINT count, int *retry )
{
	CURL_PRIV *priv = io->priv;
	int used = cb_get_used( &priv->cb );
	int copy = 0;
	
DBG2 serprintf("curl_read( %lld/%d/%d )", io->pos, count, used );
	
	// how much is in the buffer
	if( used ){
		copy = MIN( count, used );
		// get from buffer
		cb_read( &priv->cb, buffer, copy );
DBG2 serprintf("  circ: %d ", copy );
		buffer += copy;
		count  -= copy; 
		
		io->pos += copy;
	}
	
	if( priv->done ) {
DBG2 serprintf("CURL: at end! pos: %8lld  left %d\r\n", io->pos, used );
		// we are done, no more!
		io->size = io->pos;
		return copy;
	}
	
	int start = atime();
	static int last = 0;
	if( !last )
		last = start;
	// more data needed, get from curl
	if( count ) {
DBG2 serprintf("  curl: %d", count );
		UCHAR *buf_start  = buffer;
		priv->read_buffer = buffer;
		priv->read_count  = count;

		while( 1 ) {
DBGC serprintf("[%3d] rest %6d", atime() - last, priv->read_count);
			last = atime();
			int perform = 0;
			
			if( use_select ) {
				int max_fd = -1;
				fd_set read_fds, write_fds, except_fds;

				struct timeval maxwait;
				maxwait.tv_sec  = 1;
				maxwait.tv_usec = 0;

				// wait for something to happen
				FD_ZERO( &read_fds   );
				FD_ZERO( &write_fds  );
				FD_ZERO( &except_fds );

				curl_multi_fdset( priv->multi_handle, &read_fds, &write_fds, &except_fds, &max_fd);

DBGC serprintf("  max: %2d", max_fd);
				if( max_fd == -1 ) {
					perform = 1;
				} else {
					// blocking wait for something to happen or timeout
					int ret = select( max_fd + 1, &read_fds, &write_fds, &except_fds, &maxwait);
DBGC serprintf("  ret %d  %8d/%8d", ret, maxwait.tv_sec, maxwait.tv_usec );
					if (ret > 0 || (!maxwait.tv_sec && !maxwait.tv_usec) ) {
						perform = 1;
					}
				}
			} else {
				perform = 1;
				msec_sleep( 10 );
			}
			if( perform && !curl_fail ) {
				int running;
				int res;
				do{ 
DBGC serprintf(" [");
					int start = atime();
					res = curl_multi_perform(priv->multi_handle, &running);		
DBGC serprintf("] t %3d  res %d  run %d", atime() - start, res, running );
					if( _check_msgs( priv, &priv->done, retry ) ) {
DBGC serprintf("\r\n" );
						// something is wrong, abort!
						return -1;
					} else if( priv->done ) {
							int bytes_read = priv->read_buffer - buffer;
							io->pos += bytes_read;
serprintf("CURL: done! pos: %8lld\r\n", io->pos );
							return copy + bytes_read;
					} 
				} while ( res == -1 );

				if( !running ) {
DBGC serprintf("\r\n" );
					break;
				}
				if( priv->read_count <= 0 ) {
DBGC serprintf("  %d\r\n", atime() - start );
					break;
				}
			}
DBGC serprintf("\r\n" );
			if( io->abort ) {
				int abort = io->abort( io->abort_ctx );
				if( abort == STREAM_BUFFER_ABORT_FINAL ) {
serprintf("CURL: abort FINAL\r\n" );				
					return -1 * STREAM_BUFFER_ABORT_FINAL;
				} else if ( abort == STREAM_BUFFER_ABORT_THIS ) {
serprintf("CURL: abort THIS: %d\r\n", priv->read_buffer - buf_start );				
					break;					
				}
			}
		}
		io->pos += priv->read_buffer - buf_start;
		copy    += priv->read_buffer - buf_start;
	}
DBG2 serprintf("  -> %8lld\r\n", io->pos );

	return copy;
}

static int _read( STREAM_IO *io, UCHAR *buffer, UINT count )
{
	UINT64 pos = io->pos;
	
	int retry;
	int ret = _try_read( io, buffer, count, &retry );

	if( ret == -1 ) {
		if( retry ) {
serprintf("CURL: CURLE_GOT_NOTHING from server! trying to recover\n");
			io->pos = 0;
			_seek( io, pos );
			ret = _try_read( io, buffer, count, &retry );
			if( ret == -1 ) {
serprintf("CURL: retry failed, giving up!\n");
				return -1;
			}
		}
		
	}
	return ret;
}

static int _write( STREAM_IO *io, UCHAR *buffer, UINT count)
{
	return 0;
}

static int _power_state( STREAM_IO *io )
{
	return 0;
}

static int _seekable( STREAM_IO *io )
{
	CURL_PRIV *priv = io->priv;
	return priv->seekable;
}

static int _sleepable( STREAM_IO *io, int *sleep_time, int *wake_time )
{
	if ( sleep_time )
		*sleep_time = 0;
	if ( wake_time )
		*wake_time = 0;
	return 0;
}

static int _delete( STREAM_IO *io )
{
	if( io && io->priv )
		afree( io->priv );
	if( io	)
		afree( io );
	return 0;
} 

static int _can_read( STREAM_IO *io, UINT64 pos, UINT64 count )
{
	return 1;
} 

static STREAM_IO *_new( STREAM_URL *src ) 
{
	STREAM_IO *io = stream_io_new( src );
	if( !io )
		return NULL;
	
	io->delete      = _delete;
	io->open        = _open;
	io->is_open     = _is_open;
	io->close       = _close;
	io->get_size    = stream_io_get_size;
	io->seek        = _seek;
	io->read        = _read;
	io->write       = _write;
	io->power_state = _power_state;
	io->seekable    = _seekable;
	io->sleepable   = _sleepable;
	io->can_read	= _can_read;
	io->can_abort   = 1;

	// allocate private data
	if( !(io->priv = (CURL_PRIV*)amalloc( sizeof( CURL_PRIV ) ) ) ) {
		afree( io );
		return NULL;
	}
	memset( io->priv, 0, sizeof( CURL_PRIV ) );
	
	return io;	 
}

static char http_proto[] = "http://";
STREAM_REGISTER_IO( http_proto, _new, STREAM_IO_NONLOCAL, ETYPE_NONE );

static char https_proto[] = "https://";
STREAM_REGISTER_IO( https_proto, _new, STREAM_IO_NONLOCAL, ETYPE_NONE );

#endif
