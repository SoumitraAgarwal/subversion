/*
 * main.c :  Main control function for svnserve
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_general.h>
#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_getopt.h>
#include <apr_network_io.h>
#include <apr_signal.h>
#include <apr_thread_proc.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_ra_svn.h"
#include "svn_utf.h"
#include "svn_path.h"

#include "server.h"

/* The strategy for handling incoming connections.  Some of these may be
   unavailable due to platform limitations. */
enum connection_handling_mode {
  connection_mode_fork,   /* Create a process per connection */
  connection_mode_thread, /* Create a thread per connection */
  connection_mode_single  /* One connection at a time in this process */
};

#if APR_HAS_FORK
#if APR_HAS_THREADS

#define CONNECTION_DEFAULT connection_mode_fork
#define CONNECTION_USAGE "|-T"
#define CONNECTION_OPT "T"

#else /* ! APR_HAS_THREADS */

#define CONNECTION_DEFAULT connection_mode_fork
#define CONNECTION_USAGE
#define CONNECTION_OPT

#endif /* ! APR_HAS_THREADS */
#elif APR_HAS_THREADS /* and ! APR_HAS_FORK */

#define CONNECTION_DEFAULT connection_mode_thread
#define CONNECTION_USAGE
#define CONNECTION_OPT

#else /* ! APR_HAS_THREADS and ! APR_HAS_FORK */

#define CONNECTION_DEFAULT connection_mode_single
#define CONNECTION_USAGE
#define CONNECTION_OPT

#endif

static void usage(const char *progname)
{
  if (!progname)
    progname = "svn-server";
  fprintf(stderr,
          "Usage: %s [-X|-d|-t|-R|-u" CONNECTION_USAGE "] [-r root]\n",
          progname);
  exit(1);
}

#if APR_HAS_FORK
static void sigchld_handler(int signo)
{
  /* Nothing to do; we just need to interrupt the accept(). */
}
#endif

/* In tunnel or inetd mode, we don't want hook scripts corrupting the
 * data stream by sending data to stdout, so we need to redirect
 * stdout somewhere else.  Sending it to stderr is acceptable; sending
 * it to /dev/null is another option, but apr doesn't provide a way to
 * do that without also detaching from the controlling terminal.
 */
static apr_status_t redirect_stdout(void *arg)
{
  apr_pool_t *pool = arg;
  apr_file_t *out_file, *err_file;

  apr_file_open_stdout(&out_file, pool);
  apr_file_open_stderr(&err_file, pool);
  return apr_file_dup2(out_file, err_file, pool);
}

int main(int argc, const char *const *argv)
{
  svn_boolean_t listen_once = FALSE, daemon_mode = FALSE, tunnel_mode = FALSE;
  svn_boolean_t read_only = FALSE, believe_username = FALSE;
  apr_socket_t *sock, *usock;
  apr_file_t *in_file, *out_file;
  apr_sockaddr_t *sa;
  apr_pool_t *pool;
  apr_pool_t *connection_pool;
  svn_error_t *err;
  apr_getopt_t *os;
  char opt, errbuf[256];
  const char *arg, *root = "/";
  apr_status_t status;
  svn_ra_svn_conn_t *conn;
  apr_proc_t proc;
  enum connection_handling_mode handling_mode = CONNECTION_DEFAULT;

  /* Initialize the app. */
  if (svn_cmdline_init ("svn", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  /* Create our top-level pool. */
  pool = svn_pool_create(NULL);

  apr_getopt_init(&os, pool, argc, argv);

  while (1)
    {
      status = apr_getopt(os, "dtXr:Ru" CONNECTION_OPT, &opt, &arg);
      if (APR_STATUS_IS_EOF(status))
        break;
      if (status != APR_SUCCESS)
        usage(argv[0]);
      switch (opt)
        {
        case 'd':
          daemon_mode = TRUE;
          break;

        case 't':
          tunnel_mode = TRUE;
          break;

        case 'X':
          listen_once = TRUE;
          break;

        case 'r':
          SVN_INT_ERR(svn_utf_cstring_to_utf8(&root, arg, pool));
          root = svn_path_internal_style(root, pool);
          SVN_INT_ERR(svn_path_get_absolute(&root, root, pool));
          break;

        case 'R':
          read_only = TRUE;
          break;

        case 'T':
          handling_mode = connection_mode_thread;
          break;

        case 'u':
          believe_username = TRUE;
          break;
        }
    }
  if (os->ind != argc)
    usage(argv[0]);

  if (!daemon_mode && !listen_once)
    {
      apr_pool_cleanup_register(pool, pool, apr_pool_cleanup_null,
                                redirect_stdout);
      apr_file_open_stdin(&in_file, pool);
      apr_file_open_stdout(&out_file, pool);
      conn = svn_ra_svn_create_conn(NULL, in_file, out_file, pool);
      svn_error_clear(serve(conn, root, tunnel_mode, read_only, 
                            believe_username, pool));
      exit(0);
    }

#if APR_HAS_THREADS
  /* Initialize the threaded server */
  init_threads(pool);
#endif

#ifdef MAX_SECS_TO_LINGER
  /* ### old APR interface */
  status = apr_socket_create(&sock, APR_INET, SOCK_STREAM, pool);
#else
  status = apr_socket_create(&sock, APR_INET, SOCK_STREAM, APR_PROTO_TCP,
                             pool);
#endif
  if (status)
    {
      fprintf(stderr, "Can't create server socket: %s\n",
              apr_strerror(status, errbuf, sizeof(errbuf)));
      exit(1);
    }

  /* Prevents "socket in use" errors when server is killed and quickly
   * restarted. */
  apr_socket_opt_set(sock, APR_SO_REUSEADDR, 1);

  apr_sockaddr_info_get(&sa, NULL, APR_INET, SVN_RA_SVN_PORT, 0, pool);
  status = apr_socket_bind(sock, sa);
  if (status)
    {
      fprintf(stderr, "Can't bind server socket: %s\n",
              apr_strerror(status, errbuf, sizeof(errbuf)));
      exit(1);
    }

  apr_socket_listen(sock, 7);

#if APR_HAS_FORK
  if (!listen_once)
    apr_proc_detach(APR_PROC_DETACH_DAEMONIZE);

  apr_signal(SIGCHLD, sigchld_handler);
#endif

  while (1)
    {
      /* Non-standard pool handling.  The main thread never blocks to join
         the connection threads so it cannot clean up after each one.  So
         separate pools, that can be cleared at thread exit, are used */
      connection_pool = svn_pool_create(NULL);

      status = apr_socket_accept(&usock, sock, connection_pool);
      if (handling_mode == connection_mode_fork)
        {
          /* Collect any zombie child processes. */
          while (apr_proc_wait_all_procs(&proc, NULL, NULL, APR_NOWAIT,
                                         connection_pool) == APR_CHILD_DONE)
            ;
        }
      if (APR_STATUS_IS_EINTR(status))
        {
          svn_pool_destroy(connection_pool);
          continue;
        }
      if (status)
        {
          fprintf(stderr, "Can't accept client connection: %s\n",
                  apr_strerror(status, errbuf, sizeof(errbuf)));
          exit(1);
        }

      conn = svn_ra_svn_create_conn(usock, NULL, NULL, connection_pool);

      if (listen_once)
        {
          err = serve(conn, root, FALSE, read_only, believe_username, 
                      connection_pool);

          if (listen_once && err
              && err->apr_err != SVN_ERR_RA_SVN_CONNECTION_CLOSED)
            svn_handle_error(err, stdout, FALSE);

          apr_socket_close(usock);
          apr_socket_close(sock);
          exit(0);
        }

      switch (handling_mode)
        {
        case connection_mode_fork:
#if APR_HAS_FORK
          status = apr_proc_fork(&proc, connection_pool);
          if (status == APR_INCHILD)
            {
              svn_error_clear(serve(conn, root, FALSE, read_only,
                                    believe_username, connection_pool));
              apr_socket_close(usock);
              exit(0);
            }
          else if (status == APR_INPARENT)
            {
              apr_socket_close(usock);
            }
          else
            {
              /* Log an error, when we support logging. */
              apr_socket_close(usock);
            }
          svn_pool_destroy(connection_pool);
#endif
          break;

        case connection_mode_thread:
          /* Pass the request off to a worker thread. */
#if APR_HAS_THREADS
          serve_thread(conn, root, read_only, believe_username,
                       pool, connection_pool);
#endif
          break;

        case connection_mode_single:
          /* Serve one connection at a time. */
          svn_error_clear(serve(conn, root, FALSE, read_only,
                                believe_username, connection_pool));
          svn_pool_destroy(connection_pool);
        }
    }

  return 0;
}
