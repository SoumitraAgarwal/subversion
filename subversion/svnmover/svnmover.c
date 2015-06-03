/*
 * svnmover.c: Concept Demo for Move Tracking and Branching
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <apr_lib.h>

#include "svn_private_config.h"
#include "svn_hash.h"
#include "svn_iter.h"
#include "svn_client.h"
#include "svn_cmdline.h"
#include "svn_config.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_string.h"
#include "svn_subst.h"
#include "svn_utf.h"
#include "svn_version.h"

#include "private/svn_cmdline_private.h"
#include "private/svn_subr_private.h"
#include "private/svn_editor3e.h"
#include "private/svn_ra_private.h"
#include "private/svn_string_private.h"
#include "private/svn_sorts_private.h"
#include "private/svn_token.h"

#define HAVE_LINENOISE
#ifdef HAVE_LINENOISE
#include "../libsvn_subr/linenoise/linenoise.h"
#endif

/* Version compatibility check */
static svn_error_t *
check_lib_versions(void)
{
  static const svn_version_checklist_t checklist[] =
    {
      { "svn_client", svn_client_version },
      { "svn_subr",   svn_subr_version },
      { "svn_ra",     svn_ra_version },
      { NULL, NULL }
    };
  SVN_VERSION_DEFINE(my_version);

  return svn_ver_check_list2(&my_version, checklist, svn_ver_equal);
}

static svn_boolean_t quiet = FALSE;

/* UI mode: whether to display output in terms of paths or elements */
enum { UI_MODE_EIDS, UI_MODE_PATHS };
static int the_ui_mode = UI_MODE_EIDS;
static const svn_token_map_t ui_mode_map[]
  = { {"eids", UI_MODE_EIDS},
      {"e", UI_MODE_EIDS},
      {"paths", UI_MODE_PATHS},
      {"p", UI_MODE_PATHS},
      {NULL, SVN_TOKEN_UNKNOWN} };

/* Is BRANCH1 the same branch as BRANCH2? Compare by full branch-ids; don't
   require identical branch objects. */
#define BRANCH_IS_SAME_BRANCH(branch1, branch2, scratch_pool) \
  (strcmp(svn_branch_get_id(branch1, scratch_pool), \
          svn_branch_get_id(branch2, scratch_pool)) == 0)

/*  */
__attribute__((format(printf, 1, 2)))
static void
notify(const char *fmt,
       ...)
{
  va_list ap;

  if (! quiet)
    {
      va_start(ap, fmt);
      vprintf(fmt, ap);
      va_end(ap);
      printf("\n");
    }
}

#define SVN_CL__LOG_SEP_STRING \
  "------------------------------------------------------------------------\n"

/* ====================================================================== */

typedef struct svnmover_wc_t
{
  apr_pool_t *pool;
  const char *repos_root_url;
  /*const char *anchor_repos_relpath;*/
  svn_revnum_t head_revision;
  svn_revnum_t base_revision;

  svn_ra_session_t *ra_session;
  svn_editor3_t *editor;
  svn_branch_revision_root_t *edit_txn;
  svn_client_ctx_t *ctx;

} svnmover_wc_t;

/* Update the WC to revision BASE_REVISION (SVN_INVALID_REVNUM means HEAD).
 *
 * Requires these fields in WC:
 *   head_revision
 *   repos_root_url
 *   ra_session
 *   pool
 *
 * Initializes these fields in WC:
 *   base_revision
 *   edit_txn
 *   editor
 *
 * Assumes there are no changes in the WC: throws away the existing txn
 * and starts a new one.
 */
static svn_error_t *
wc_checkout(svnmover_wc_t *wc,
            svn_revnum_t base_revision,
            apr_pool_t *scratch_pool)
{
  const char *branch_info_dir = NULL;
  svn_editor3__shim_fetch_func_t fetch_func;
  void *fetch_baton;

  /* Validate and store the new base revision number */
  if (! SVN_IS_VALID_REVNUM(base_revision))
    wc->base_revision = wc->head_revision;
  else if (base_revision > wc->head_revision)
    return svn_error_createf(SVN_ERR_FS_NO_SUCH_REVISION, NULL,
                             _("No such revision %ld (HEAD is %ld)"),
                             base_revision, wc->head_revision);
  else
    wc->base_revision = base_revision;

  /* Choose whether to store branching info in a local dir or in revprops.
     (For now, just to exercise the options, we choose local files for
     RA-local and revprops for a remote repo.) */
  if (strncmp(wc->repos_root_url, "file://", 7) == 0)
    {
      const char *repos_dir;

      SVN_ERR(svn_uri_get_dirent_from_file_url(&repos_dir, wc->repos_root_url,
                                               scratch_pool));
      branch_info_dir = svn_dirent_join(repos_dir, "branch-info", scratch_pool);
    }

  /* Get a mutable transaction based on that rev. (This implementation
     re-reads all the move-tracking data from the repository.) */
  SVN_ERR(svn_ra_load_branching_state(&wc->edit_txn,
                                      &fetch_func, &fetch_baton,
                                      wc->ra_session, branch_info_dir,
                                      wc->base_revision,
                                      wc->pool, scratch_pool));
  SVN_ERR(svn_editor3_in_memory(&wc->editor,
                                wc->edit_txn,
                                fetch_func, fetch_baton,
                                wc->pool));

  return SVN_NO_ERROR;
}

/* Create a simulated WC, in memory.
 *
 * Initializes these fields in WC:
 *   head_revision
 *   repos_root_url
 *   ra_session
 *   made_changes
 *   ctx
 *   pool
 *
 * BASE_REVISION is the revision to work on, or SVN_INVALID_REVNUM for HEAD.
 */
static svn_error_t *
wc_create(svnmover_wc_t **wc_p,
          const char *anchor_url,
          svn_revnum_t base_revision,
          svn_client_ctx_t *ctx,
          apr_pool_t *result_pool,
          apr_pool_t *scratch_pool)
{
  apr_pool_t *wc_pool = svn_pool_create(result_pool);
  svnmover_wc_t *wc = apr_pcalloc(wc_pool, sizeof(*wc));

  wc->pool = wc_pool;
  wc->ctx = ctx;

  SVN_ERR(svn_client_open_ra_session2(&wc->ra_session, anchor_url,
                                      NULL /* wri_abspath */, ctx,
                                      wc_pool, scratch_pool));

  SVN_ERR(svn_ra_get_repos_root2(wc->ra_session, &wc->repos_root_url,
                                 result_pool));
  SVN_ERR(svn_ra_get_latest_revnum(wc->ra_session, &wc->head_revision,
                                   scratch_pool));

  SVN_ERR(wc_checkout(wc, base_revision, scratch_pool));
  *wc_p = wc;
  return SVN_NO_ERROR;
}

/* Replay differences between S_LEFT and S_RIGHT into EDITOR:EDIT_BRANCH.
 *
 * S_LEFT and/or S_RIGHT may be null meaning an empty set.
 *
 * Non-recursive: single branch only.
 */
static svn_error_t *
subtree_replay(svn_editor3_t *editor,
               svn_branch_state_t *edit_branch,
               svn_branch_subtree_t *s_left,
               svn_branch_subtree_t *s_right,
               apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_left_right;
  apr_hash_index_t *hi;

  if (! s_left)
    s_left = svn_branch_subtree_create(NULL, 0 /*root_eid*/, scratch_pool);
  if (! s_right)
    s_right = svn_branch_subtree_create(NULL, 0 /*root_eid*/, scratch_pool);

  SVN_ERR(svn_branch_subtree_differences(&diff_left_right,
                                         editor, s_left, s_right,
                                         scratch_pool, scratch_pool));

  /* Go through the per-element differences. */
  for (hi = apr_hash_first(scratch_pool, diff_left_right);
       hi; hi = apr_hash_next(hi))
    {
      int eid = svn_int_hash_this_key(hi);
      svn_branch_el_rev_content_t **e_pair = apr_hash_this_val(hi);
      svn_branch_el_rev_content_t *e0 = e_pair[0], *e1 = e_pair[1];

      if (e0 || e1)
        {
          if (e0 && e1)
            {
              SVN_DBG(("replay: alter e%d", eid));
              SVN_ERR(svn_editor3_alter(editor,
                                        edit_branch, eid,
                                        e1->parent_eid, e1->name,
                                        e1->payload));
            }
          else if (e0)
            {
              SVN_DBG(("replay: delete e%d", eid));
              SVN_ERR(svn_editor3_delete(editor,
                                         edit_branch, eid));
            }
          else
            {
              SVN_DBG(("replay: instan. e%d", eid));
              SVN_ERR(svn_editor3_alter(editor,
                                        edit_branch, eid,
                                        e1->parent_eid, e1->name,
                                        e1->payload));
            }
        }
    }

  return SVN_NO_ERROR;
}

/* Replay differences between S_LEFT and S_RIGHT into EDITOR:EDIT_BRANCH.
 *
 * S_LEFT or S_RIGHT (but not both) may be null meaning an empty set.
 *
 * Recurse into subbranches.
 */
static svn_error_t *
svn_branch_replay(svn_editor3_t *editor,
                  svn_branch_state_t *edit_branch,
                  svn_branch_subtree_t *s_left,
                  svn_branch_subtree_t *s_right,
                  apr_pool_t *scratch_pool)
{
  assert((s_left && s_right) ? (s_left->root_eid == s_right->root_eid)
                             : (s_left || s_right));

  if (s_right)
    {
      /* Replay this branch */
      SVN_ERR(subtree_replay(editor, edit_branch, s_left, s_right,
                             scratch_pool));
    }
  else
    {
      /* deleted branch LEFT */
      /* nothing to do -- it will go away because we deleted the outer-branch
         element where it was attached */
    }

  /* Replay its subbranches, recursively.
     (If we're deleting the current branch, we don't also need to
     explicitly delete its subbranches... do we?) */
  if (s_right)
    {
      apr_hash_t *subtrees_all;
      apr_hash_index_t *hi;

      subtrees_all = s_left ? apr_hash_overlay(scratch_pool,
                                               s_left->subbranches,
                                               s_right->subbranches)
                            : s_right->subbranches;

      for (hi = apr_hash_first(scratch_pool, subtrees_all);
           hi; hi = apr_hash_next(hi))
        {
          int this_eid = svn_int_hash_this_key(hi);
          svn_branch_subtree_t *this_s_left
            = s_left ? svn_int_hash_get(s_left->subbranches, this_eid) : NULL;
          svn_branch_subtree_t *this_s_right
            = s_right ? svn_int_hash_get(s_right->subbranches, this_eid) : NULL;
          svn_branch_state_t *edit_subbranch;

          /* If the subbranch is to be added, first create a new edit branch;
             if it is to be edited or deleted, then look up the edit branch */
          if (this_s_left)
            {
              edit_subbranch = svn_branch_get_subbranch_at_eid(
                                 edit_branch, this_eid, scratch_pool);
              /*SVN_DBG(("replaying subbranch: found br %s (left=%s right=%s)",
                       svn_branch_get_id(edit_subbranch, scratch_pool),
                       svn_branch_get_id(branch_l, scratch_pool),
                       branch_r ? svn_branch_get_id(branch_r, scratch_pool) : "<nil>"));*/
            }
          else
            {
              edit_subbranch = svn_branch_add_new_branch(
                                 edit_branch, this_eid,
                                 this_s_right->root_eid, scratch_pool);
              /*SVN_DBG(("replaying subbranch: added br %s (right=%s)",
                       svn_branch_get_id(edit_subbranch, scratch_pool),
                       branch_r ? svn_branch_get_id(branch_r, scratch_pool) : "<nil>"));*/
            }

          /* recurse */
          SVN_ERR(svn_branch_replay(editor, edit_subbranch,
                                    this_s_left, this_s_right, scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}

/* Replay differences between LEFT_TXN and RIGHT_TXN into EDIT_ROOT_BRANCH.
 * (Recurse into subbranches.)
 */
static svn_error_t *
replay(svn_editor3_t *editor,
       svn_branch_state_t *edit_root_branch,
       svn_branch_revision_root_t *left_txn,
       svn_branch_revision_root_t *right_txn,
       apr_pool_t *scratch_pool)
{
  svn_branch_subtree_t *s_left
    = svn_branch_get_subtree(left_txn->root_branch,
                             left_txn->root_branch->root_eid, scratch_pool);
  svn_branch_subtree_t *s_right
    = svn_branch_get_subtree(right_txn->root_branch,
                             right_txn->root_branch->root_eid, scratch_pool);

  SVN_ERR(svn_branch_replay(editor, edit_root_branch,
                            s_left, s_right, scratch_pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool);

/* Baton for commit_callback(). */
typedef struct commit_callback_baton_t
{
  svn_branch_revision_root_t *edit_txn;
  svn_editor3_t *editor;

  /* just-committed revision */
  svn_revnum_t revision;
} commit_callback_baton_t;

static svn_error_t *
display_diff_of_commit(const commit_callback_baton_t *ccbb,
                       apr_pool_t *scratch_pool);

/* Commit the changes from WC into the repository.
 *
 * Open a new commit txn to the repo. Replay the changes from WC into it.
 *
 * Set WC->head_revision and *NEW_REV_P to the committed revision number.
 *
 * If there are no changes to commit, set *NEW_REV_P to SVN_INVALID_REVNUM
 * and do not make a commit and do not change WC->head_revision.
 *
 * NEW_REV_P may be null if not wanted.
 */
static svn_error_t *
wc_commit(svn_revnum_t *new_rev_p,
          svnmover_wc_t *wc,
          apr_hash_t *revprops,
          apr_pool_t *scratch_pool)
{
  const char *branch_info_dir = NULL;
  svn_branch_revision_root_t *left_txn
    = svn_array_get(wc->edit_txn->repos->rev_roots, (int)wc->base_revision);
  svn_branch_revision_root_t *right_txn = wc->edit_txn;
  svn_branch_revision_root_t *commit_txn;
  svn_editor3_t *commit_editor;
  commit_callback_baton_t ccbb;
  svn_boolean_t change_detected;

  /* Choose whether to store branching info in a local dir or in revprops.
     (For now, just to exercise the options, we choose local files for
     RA-local and revprops for a remote repo.) */
  if (strncmp(wc->repos_root_url, "file://", 7) == 0)
    {
      const char *repos_dir;

      SVN_ERR(svn_uri_get_dirent_from_file_url(&repos_dir, wc->repos_root_url,
                                               scratch_pool));
      branch_info_dir = svn_dirent_join(repos_dir, "branch-info", scratch_pool);
    }

  /* Start a new editor for the commit. */
  SVN_ERR(svn_ra_get_commit_editor_ev3(wc->ra_session,
                                       &commit_txn, &commit_editor,
                                       revprops,
                                       commit_callback, &ccbb,
                                       NULL /*lock_tokens*/, FALSE /*keep_locks*/,
                                       branch_info_dir,
                                       scratch_pool));
  SVN_ERR(svn_editor3__change_detection_editor(&commit_editor,
                                               &change_detected,
                                               commit_editor,
                                               scratch_pool));
  ccbb.edit_txn = commit_txn;
  ccbb.editor = commit_editor;
  /*SVN_ERR(svn_editor3__get_debug_editor(&wc->editor, wc->editor, scratch_pool));*/

  SVN_ERR(replay(commit_editor, commit_txn->root_branch,
                 left_txn,
                 right_txn,
                 scratch_pool));
  if (change_detected)
    {
      SVN_ERR(svn_editor3_complete(commit_editor));
      SVN_ERR(display_diff_of_commit(&ccbb, scratch_pool));

      wc->head_revision = ccbb.revision;
      if (new_rev_p)
        *new_rev_p = ccbb.revision;
    }
  else
    {
      SVN_ERR(svn_editor3_abort(commit_editor));
      if (new_rev_p)
        *new_rev_p = SVN_INVALID_REVNUM;
    }

  return SVN_NO_ERROR;
}

typedef enum action_code_t {
  ACTION_DIFF,
  ACTION_LOG,
  ACTION_LIST_BRANCHES,
  ACTION_LIST_BRANCHES_R,
  ACTION_LS,
  ACTION_BRANCH,
  ACTION_BRANCH_INTO,
  ACTION_MKBRANCH,
  ACTION_MERGE,
  ACTION_MV,
  ACTION_MKDIR,
  ACTION_PUT_FILE,
  ACTION_CP,
  ACTION_RM,
  ACTION_COMMIT,
  ACTION_UPDATE,
  ACTION_STATUS,
  ACTION_REVERT
} action_code_t;

typedef struct action_defn_t {
  enum action_code_t code;
  const char *name;
  int num_args;
  const char *args_help;
  const char *help;
} action_defn_t;

#define NL "\n                           "
static const action_defn_t action_defn[] =
{
  {ACTION_LIST_BRANCHES,    "branches", 1, "PATH",
    "list all branches rooted at the same element as PATH"},
  {ACTION_LIST_BRANCHES_R,  "ls-br-r", 0, "",
    "list all branches, recursively"},
  {ACTION_LS,               "ls", 1, "PATH",
    "list elements in the branch found at PATH"},
  {ACTION_LOG,              "log", 2, "FROM@REV TO@REV",
    "show per-revision diffs between FROM and TO"},
  {ACTION_BRANCH,           "branch", 2, "SRC DST",
    "branch the branch-root or branch-subtree at SRC" NL
    "to make a new branch at DST"},
  {ACTION_BRANCH_INTO,      "branch-into", 2, "SRC DST",
    "make a copy of the branch-root or branch-subtree at SRC" NL
    "appear at DST in the existing branch that contains DST" NL
    "(like merging the creation of the subtree at SRC to DST)"},
  {ACTION_MKBRANCH,         "mkbranch", 1, "ROOT",
    "make a directory that's the root of a new subbranch"},
  {ACTION_DIFF,             "diff", 2, "LEFT@REV RIGHT@REV",
    "show differences from subtree LEFT to subtree RIGHT"},
  {ACTION_MERGE,            "merge", 3, "FROM TO YCA@REV",
    "3-way merge YCA->FROM into TO"},
  {ACTION_CP,               "cp", 2, "REV SRC DST",
    "copy SRC@REV to DST"},
  {ACTION_MV,               "mv", 2, "SRC DST",
    "move SRC to DST"},
  {ACTION_RM,               "rm", 1, "PATH",
    "delete PATH"},
  {ACTION_MKDIR,            "mkdir", 1, "PATH",
    "create new directory PATH"},
  {ACTION_PUT_FILE,         "put", 2, "LOCAL_FILE PATH",
    "add or modify file PATH with text copied from" NL
    "LOCAL_FILE (use \"-\" to read from standard input)"},
  {ACTION_COMMIT,           "commit", 0, "",
    "commit the changes"},
  {ACTION_UPDATE,           "update", 1, ".@REV",
    "update to revision REV, keeping local changes"},
  {ACTION_STATUS,           "status", 0, "",
    "same as 'diff .@base .'"},
  {ACTION_REVERT,           "revert", 0, "",
    "revert all uncommitted changes"},
};

typedef struct action_t {
  action_code_t action;

  /* argument revisions */
  svn_opt_revision_t rev_spec[3];

  /* argument paths */
  const char *relpath[3];
} action_t;

/* ====================================================================== */

/* Find the deepest branch in the repository of which REVNUM:RRPATH is
 * either the root element or a normal, non-sub-branch element.
 *
 * RRPATH is a repository-relative path. REVNUM is a revision number, or
 * SVN_INVALID_REVNUM meaning the current txn.
 *
 * Return the location of the element in that branch, or with
 * EID=-1 if no element exists there.
 *
 * The result will never be NULL, as every path is within at least the root
 * branch.
 */
static svn_error_t *
find_el_rev_by_rrpath_rev(svn_branch_el_rev_id_t **el_rev_p,
                          svnmover_wc_t *wc,
                          svn_revnum_t revnum,
                          const char *rrpath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  if (SVN_IS_VALID_REVNUM(revnum))
    {
      const svn_branch_repos_t *repos = wc->edit_txn->repos;

      SVN_ERR(svn_branch_repos_find_el_rev_by_path_rev(el_rev_p,
                                                       rrpath, revnum, repos,
                                                       result_pool,
                                                       scratch_pool));
    }
  else
    {
      svn_branch_el_rev_id_t *el_rev = apr_palloc(result_pool, sizeof(*el_rev));

      svn_branch_find_nested_branch_element_by_rrpath(
        &el_rev->branch, &el_rev->eid,
        wc->edit_txn->root_branch, rrpath, scratch_pool);
      el_rev->rev = SVN_INVALID_REVNUM;
      *el_rev_p = el_rev;
    }
  SVN_ERR_ASSERT(*el_rev_p);
  return SVN_NO_ERROR;
}

/* Return a string suitable for appending to a displayed element name or
 * element id to indicate that it is a subbranch root element for SUBBRANCH.
 * Return "" if SUBBRANCH is null.
 */
static const char *
branch_str(svn_branch_state_t *subbranch,
           apr_pool_t *result_pool)
{
  if (subbranch)
    return apr_psprintf(result_pool,
                      " (branch %s)",
                      svn_branch_get_id(subbranch, result_pool));
  return "";
}

/* Return a string suitable for appending to a displayed element name or
 * element id to indicate that BRANCH:EID is a subbranch root element.
 * Return "" if the element is not a subbranch root element.
 */
static const char *
subbranch_str(svn_branch_state_t *branch,
              int eid,
              apr_pool_t *result_pool)
{
  svn_branch_state_t *subbranch
    = svn_branch_get_subbranch_at_eid(branch, eid, result_pool);

  return branch_str(subbranch, result_pool);
}

/* A flat snapshot of a branch hierarchy.
 *
 * A 'flat' branch hierarchy means one in which all elements are in a
 * single hierarchy of paths. Every element has a relative path within
 * the branch, determined by its directory hierarchy. Every element in a
 * hierarchy of branches has a complete path relative to the repo root.
 *
 * A flat branch may but does not necessarily represent a single-revision
 * slice of a branch hierarchy.
 */
typedef struct flat_branch_t
{
  /* The subtree hierarchy corresponding to this flat-branch */
  svn_branch_subtree_t *s;
  /* The repository-root-relative path to the root of this flat-branch */
  const char *rrpath;
  /* The branch id of the branch of which this flat-branch is a snapshot */
  const char *bid;
} flat_branch_t;

/* Create and return a new flat-branch.
 */
static flat_branch_t *
flat_branch_create(svn_branch_subtree_t *s,
                   const char *rrpath,
                   const char *bid,
                   apr_pool_t *result_pool)
{
  flat_branch_t *fb = apr_pcalloc(result_pool, sizeof(*fb));

  fb->s = s;
  fb->rrpath = rrpath;
  fb->bid = bid;
  return fb;
}

/* Return the root EID of FB.
 */
static int
flat_branch_get_root_eid(flat_branch_t *fb)
{
  return fb->s->root_eid;
}

/* Create and return a new flat-branch that is the sub-branch of FB at EID,
 * or NULL if there is no subbranch of FB at EID.
 */
static flat_branch_t *
flat_branch_get_subbranch_at_eid(flat_branch_t *fb,
                                 int eid,
                                 apr_pool_t *result_pool)
{
  svn_branch_subtree_t *s
    = svn_branch_subtree_get_subbranch_at_eid(fb->s, eid, result_pool);
  const char *rrpath;
  const char *bid;
  flat_branch_t *sub_fb;
  
  if (! s)
    return NULL;

  rrpath = svn_relpath_join(fb->rrpath,
                            svn_branch_subtree_get_path_by_eid(fb->s, eid,
                                                               result_pool),
                            result_pool);
  bid = fb->bid ? apr_psprintf(result_pool, "B%d", eid)
                : apr_psprintf(result_pool, "%s.%d", fb->bid, eid);
  sub_fb = flat_branch_create(s, rrpath, bid, result_pool);
  return sub_fb;
}

/* Return the immediate subbranches of FB, as a mapping from EID to
 * (flat_branch_t *).
 */
static apr_hash_t *
flat_branch_get_subbranches(flat_branch_t *fb,
                            apr_pool_t *result_pool)
{
  apr_pool_t *scratch_pool = result_pool;
  apr_hash_t *subbranches = apr_hash_make(result_pool);
  apr_hash_index_t *hi;

  for (hi = apr_hash_first(scratch_pool, fb->s->subbranches);
       hi; hi = apr_hash_next(hi))
    {
      int e = svn_int_hash_this_key(hi);

      svn_int_hash_set(subbranches, e,
                       flat_branch_get_subbranch_at_eid(fb, e, result_pool));
    }
  return subbranches;
}

/* Create and return a new flat-branch corresponding to the sub-branch of
 * BRANCH at EID, or NULL if there is no subbranch of BRANCH at EID.
 */
static flat_branch_t *
branch_get_flat_branch(svn_branch_state_t *branch,
                       int eid,
                       apr_pool_t *result_pool)
{
  svn_branch_subtree_t *s = svn_branch_get_subtree(branch, eid, result_pool);
  flat_branch_t *fb;

  if (! s)
    return NULL;

  fb = flat_branch_create(s,
                          svn_branch_get_rrpath_by_eid(branch, eid, result_pool),
                          svn_branch_get_id(branch, result_pool),
                          result_pool);
  return fb;
}

/* Return a string suitable for appending to a displayed element name or
 * element id to indicate that FB:EID is a subbranch root element.
 * Return "" if the element is not a subbranch root element.
 */
static const char *
flat_branch_subbranch_str(flat_branch_t *fb,
                          int eid,
                          apr_pool_t *result_pool)
{
  flat_branch_t *subbranch
    = flat_branch_get_subbranch_at_eid(fb, eid, result_pool);

  if (subbranch)
    return apr_psprintf(result_pool,
                        " (branch %s)", subbranch->bid);
  return "";
}

/* Return a string describing flat-branch FB, or NULL if FB is null.
 */
static const char *
flat_branch_id_and_path(flat_branch_t *fb,
                        apr_pool_t *result_pool)
{
  if (! fb)
    return NULL;
  return apr_psprintf(result_pool, "%s at /%s",
                      fb->bid, fb->rrpath);
}

/* List all elements in flat-branch FB, in path notation.
 */
static svn_error_t *
list_branch_elements(flat_branch_t *fb,
                     apr_pool_t *scratch_pool)
{
  apr_hash_t *paths_to_eid = apr_hash_make(scratch_pool);
  apr_hash_index_t *hi;
  SVN_ITER_T(int) *pi;

  for (hi = apr_hash_first(scratch_pool, fb->s->e_map);
       hi; hi = apr_hash_next(hi))
    {
      int eid = svn_int_hash_this_key(hi);
      const char *relpath = svn_branch_subtree_get_path_by_eid(fb->s, eid,
                                                               scratch_pool);

      if (relpath)
        {
          svn_hash_sets(paths_to_eid, relpath, apr_pmemdup(scratch_pool,
                                                           &eid, sizeof(eid)));
        }
    }
  for (SVN_HASH_ITER_SORTED(pi, paths_to_eid, svn_sort_compare_items_as_paths, scratch_pool))
    {
      const char *relpath = pi->key;
      int eid = *pi->val;

      printf("    %s%s\n",
             relpath[0] ? relpath : ".",
             flat_branch_subbranch_str(fb, eid, scratch_pool));
    }

  return SVN_NO_ERROR;
}

/*  */
static int
sort_compare_items_by_eid(const svn_sort__item_t *a,
                          const svn_sort__item_t *b)
{
  int eid_a = *(const int *)a->key;
  int eid_b = *(const int *)b->key;

  return eid_a - eid_b;
}

/* List all elements in branch BRANCH, in element notation.
 */
static svn_error_t *
list_branch_elements_by_eid(svn_branch_state_t *branch,
                            apr_pool_t *scratch_pool)
{
  SVN_ITER_T(svn_branch_el_rev_content_t) *pi;

  for (SVN_HASH_ITER_SORTED(pi, svn_branch_get_elements(branch),
                            sort_compare_items_by_eid, scratch_pool))
    {
      int eid = *(const int *)(pi->key);
      svn_branch_el_rev_content_t *element = pi->val;

      if (element && element->parent_eid == -1)
        {
          /* root element of this branch */
          printf("    e%-3d  %-3s .\n",
                 eid, "");
        }
      else if (element)
        {
          printf("    e%-3d e%-3d/%s%s\n",
                 eid, element->parent_eid,
                 element->name,
                 subbranch_str(branch, eid, scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}

/* Show the id and path of BRANCH. If VERBOSE is true, also list its elements.
 */
static svn_error_t *
branch_info_by_paths(flat_branch_t *fb,
                     svn_boolean_t verbose,
                     apr_pool_t *scratch_pool)
{
  const char *id_and_path = flat_branch_id_and_path(fb, scratch_pool);

  printf("  %s\n",
         id_and_path);
  if (verbose)
    SVN_ERR(list_branch_elements(fb, scratch_pool));

  return SVN_NO_ERROR;
}

/* Show the id of BRANCH. If VERBOSE is true, also list its elements.
 */
static svn_error_t *
branch_info_by_eids(svn_branch_state_t *branch,
                    svn_boolean_t verbose,
                    apr_pool_t *scratch_pool)
{
  printf("  %s root=e%d\n",
         svn_branch_get_id(branch, scratch_pool),
         branch->root_eid);
  if (verbose)
    SVN_ERR(list_branch_elements_by_eid(branch, scratch_pool));

  return SVN_NO_ERROR;
}

/* List all branches rooted at EID.
 */
static svn_error_t *
list_branches(svn_branch_revision_root_t *rev_root,
              int eid,
              svn_boolean_t verbose,
              apr_pool_t *scratch_pool)
{
  const apr_array_header_t *branches;
  SVN_ITER_T(svn_branch_state_t) *bi;

  branches = svn_branch_revision_root_get_branches(rev_root, scratch_pool);

  for (SVN_ARRAY_ITER(bi, branches, scratch_pool))
    {
      svn_branch_state_t *branch = bi->val;

      if (branch->root_eid != eid)
        continue;

      if (the_ui_mode == UI_MODE_PATHS)
        {
          flat_branch_t *fb
            = branch_get_flat_branch(branch, branch->root_eid, scratch_pool);

          SVN_ERR(branch_info_by_paths(fb, verbose, scratch_pool));
        }
      else
        {
          SVN_ERR(branch_info_by_eids(branch, verbose, scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}

/* List all branches.
 */
static svn_error_t *
list_all_branches(svn_branch_revision_root_t *rev_root,
                  svn_boolean_t verbose,
                  apr_pool_t *scratch_pool)
{
  const apr_array_header_t *branches;
  SVN_ITER_T(svn_branch_state_t) *bi;

  branches = svn_branch_revision_root_get_branches(rev_root, scratch_pool);

  printf("branches:\n");

  for (SVN_ARRAY_ITER(bi, branches, scratch_pool))
    {
      svn_branch_state_t *branch = bi->val;

      if (the_ui_mode == UI_MODE_PATHS)
        {
          flat_branch_t *fb
            = branch_get_flat_branch(branch, branch->root_eid, scratch_pool);

          SVN_ERR(branch_info_by_paths(fb, verbose, bi->iterpool));
        }
      else
        {
          SVN_ERR(branch_info_by_eids(branch, verbose, bi->iterpool));
        }
    }

  return SVN_NO_ERROR;
}

/* Options to control how strict the merge is about detecting conflicts.
 *
 * The options affect cases that, depending on the user's preference, could
 * either be considered a conflict or be merged to a deterministic result.
 *
 * The set of options is flexible and may be extended in future.
 */
typedef struct merge_conflict_policy_t
{
  /* Whether to merge delete-vs-delete */
  svn_boolean_t merge_double_delete;
  /* Whether to merge add-vs-add (with same parent/name/payload) */
  svn_boolean_t merge_double_add;
  /* Whether to merge reparent-vs-reparent (with same parent) */
  svn_boolean_t merge_double_reparent;
  /* Whether to merge rename-vs-rename (with same name) */
  svn_boolean_t merge_double_rename;
  /* Whether to merge modify-vs-modify (with same payload) */
  svn_boolean_t merge_double_modify;
  /* Possible additional controls: */
  /* merge (parent, name, props, text) independently or as a group */
  /* merge (parent, name) independently or as a group */
  /* merge (props, text) independently or as a group */
} merge_conflict_policy_t;

/* Merge the payload for one element.
 *
 * If there is no conflict, set *CONFLICT_P to FALSE and *RESULT_P to the
 * merged element; otherwise set *CONFLICT_P to TRUE and *RESULT_P to NULL.
 * Note that *RESULT_P can be null, indicating a deletion.
 *
 * This handles any case where at least one of (SIDE1, SIDE2, YCA) exists.
 *
 * Allocate the result in RESULT_POOL and/or as pointers to the inputs.
 */
static void
payload_merge(svn_element_payload_t **result_p,
              svn_boolean_t *conflict_p,
              int eid,
              svn_element_payload_t *side1,
              svn_element_payload_t *side2,
              svn_element_payload_t *yca,
              const merge_conflict_policy_t *policy,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  svn_boolean_t conflict = FALSE;
  svn_element_payload_t *result = NULL;

  if (yca && side1 && side2)
    {
      if (svn_element_payload_equal(side1, yca, scratch_pool))
        {
          result = side2;
        }
      else if (svn_element_payload_equal(side2, yca, scratch_pool))
        {
          result = side1;
        }
      else if (policy->merge_double_modify
               && svn_element_payload_equal(side1, side2, scratch_pool))
        {
          SVN_DBG(("e%d double modify: ... -> { ... | ... }",
                   eid));
          result = side1;
        }
      else
        {
          /* ### Need not conflict if can merge props and text separately. */

          SVN_DBG(("e%d conflict: payload: ... -> { ... | ... }",
                   eid));
          conflict = TRUE;
        }
    }

  *result_p = result;
  *conflict_p = conflict;
}

/* Merge the content for one element.
 *
 * If there is no conflict, set *CONFLICT_P to FALSE and *RESULT_P to the
 * merged element; otherwise set *CONFLICT_P to TRUE and *RESULT_P to NULL.
 * Note that *RESULT_P can be null, indicating a deletion.
 *
 * This handles any case where at least one of (SIDE1, SIDE2, YCA) exists.
 *
 * Allocate the result in RESULT_POOL and/or as pointers to the inputs.
 */
static void
element_merge(svn_branch_el_rev_content_t **result_p,
              svn_boolean_t *conflict_p,
              int eid,
              svn_branch_el_rev_content_t *side1,
              svn_branch_el_rev_content_t *side2,
              svn_branch_el_rev_content_t *yca,
              const merge_conflict_policy_t *policy,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  svn_boolean_t same1 = svn_branch_el_rev_content_equal(yca, side1,
                                                        scratch_pool);
  svn_boolean_t same2 = svn_branch_el_rev_content_equal(yca, side2,
                                                        scratch_pool);
  svn_boolean_t conflict = FALSE;
  svn_branch_el_rev_content_t *result = NULL;

  if (same1)
    {
      result = side2;
    }
  else if (same2)
    {
      result = side1;
    }
  else if (yca && side1 && side2)
    {
      /* All three sides are different, and all exist */
      result = apr_pmemdup(result_pool, yca, sizeof(*result));

      /* merge the parent-eid */
      if (side1->parent_eid == yca->parent_eid)
        {
          result->parent_eid = side2->parent_eid;
        }
      else if (side2->parent_eid == yca->parent_eid)
        {
          result->parent_eid = side1->parent_eid;
        }
      else if (policy->merge_double_reparent
               && side1->parent_eid == side2->parent_eid)
        {
          SVN_DBG(("e%d double reparent: e%d -> { e%d | e%d }",
                   eid, yca->parent_eid, side1->parent_eid, side2->parent_eid));
          result->parent_eid = side1->parent_eid;
        }
      else
        {
          SVN_DBG(("e%d conflict: parent: e%d -> { e%d | e%d }",
                   eid, yca->parent_eid, side1->parent_eid, side2->parent_eid));
          conflict = TRUE;
        }

      /* merge the name */
      if (strcmp(side1->name, yca->name) == 0)
        {
          result->name = side2->name;
        }
      else if (strcmp(side2->name, yca->name) == 0)
        {
          result->name = side1->name;
        }
      else if (policy->merge_double_rename
               && strcmp(side1->name, side2->name) == 0)
        {
          SVN_DBG(("e%d double rename: %s -> { %s | %s }",
                   eid, yca->name, side1->name, side2->name));
          result->name = side1->name;
        }
      else
        {
          SVN_DBG(("e%d conflict: name: %s -> { %s | %s }",
                   eid, yca->name, side1->name, side2->name));
          conflict = TRUE;
        }

      /* merge the payload */
      payload_merge(&result->payload, &conflict,
                    eid, side1->payload, side2->payload, yca->payload,
                    policy, result_pool, scratch_pool);
    }
  else if (! side1 && ! side2)
    {
      /* Double delete (as we assume at least one of YCA/SIDE1/SIDE2 exists) */
      if (policy->merge_double_delete)
        {
          SVN_DBG(("e%d double delete",
                   eid));
          result = side1;
        }
      else
        {
          SVN_DBG(("e%d conflict: delete vs. delete",
                   eid));
          conflict = TRUE;
        }
    }
  else if (side1 && side2)
    {
      /* Double add (as we already handled the case where YCA also exists) */
      /* May be allowed for equal content of a normal element (not subbranch) */
      if (policy->merge_double_add
          && side1->payload && side2->payload /* they are not subbranches */
          && svn_branch_el_rev_content_equal(side1, side2, scratch_pool))
        {
          SVN_DBG(("e%d double add",
                   eid));
          result = side1;
        }
      else
        {
          SVN_DBG(("e%d conflict: add vs. add (%s)",
                   eid,
                   svn_branch_el_rev_content_equal(side1, side2,
                                                   scratch_pool)
                     ? "same content" : "different content"));
          conflict = TRUE;
        }
    }
  else
    {
      /* The remaining cases must be delete vs. modify */
      SVN_DBG(("e%d conflict: delete vs. modify: %d -> { %d | %d }",
               eid, !!yca, !!side1, !!side2));
      conflict = TRUE;
    }

  *result_p = result;
  *conflict_p = conflict;
}

static svn_error_t *
branch_merge_subtree_r(svn_editor3_t *editor,
                       const svn_branch_el_rev_id_t *src,
                       const svn_branch_el_rev_id_t *tgt,
                       const svn_branch_el_rev_id_t *yca,
                       apr_pool_t *scratch_pool);

/* Merge the subbranch of {SRC, TGT, YCA} found at EID.
 */
static svn_error_t *
merge_subbranch(svn_editor3_t *editor,
                const svn_branch_el_rev_id_t *src,
                const svn_branch_el_rev_id_t *tgt,
                const svn_branch_el_rev_id_t *yca,
                int eid,
                apr_pool_t *scratch_pool)
{
  svn_branch_state_t *src_subbranch
    = svn_branch_get_subbranch_at_eid(src->branch, eid, scratch_pool);
  svn_branch_state_t *tgt_subbranch
    = svn_branch_get_subbranch_at_eid(tgt->branch, eid, scratch_pool);
  svn_branch_state_t *yca_subbranch
    = svn_branch_get_subbranch_at_eid(yca->branch, eid, scratch_pool);
  svn_branch_el_rev_id_t *subbr_src = NULL;
  svn_branch_el_rev_id_t *subbr_tgt = NULL;
  svn_branch_el_rev_id_t *subbr_yca = NULL;

  if (src_subbranch)
    subbr_src = svn_branch_el_rev_id_create(
                  src_subbranch, src_subbranch->root_eid,
                  src->rev, scratch_pool);
  if (tgt_subbranch)
    subbr_tgt = svn_branch_el_rev_id_create(
                  tgt_subbranch, tgt_subbranch->root_eid,
                  tgt->rev, scratch_pool);
  if (yca_subbranch)
    subbr_yca = svn_branch_el_rev_id_create(
                  yca_subbranch, yca_subbranch->root_eid,
                  yca->rev, scratch_pool);

  if (subbr_src && subbr_tgt && subbr_yca)  /* ?edit vs. ?edit */
    {
      /* subbranch possibly changed in source => merge */
      SVN_ERR(branch_merge_subtree_r(editor, subbr_src, subbr_tgt, subbr_yca,
                                     scratch_pool));
    }
  else if (subbr_src && subbr_yca)  /* ?edit vs. delete */
    {
      /* ### possible conflict (edit vs. delete) */
    }
  else if (subbr_tgt && subbr_yca)  /* delete vs. ?edit */
    {
      /* ### possible conflict (delete vs. edit) */
    }
  else if (subbr_src && subbr_tgt)  /* double add */
    {
      /* ### conflict */
    }
  else if (subbr_src)  /* added on source branch */
    {
      svn_branch_subtree_t *from_subtree
        = svn_branch_get_subtree(src_subbranch,
                                     src_subbranch->root_eid,
                                     scratch_pool);

      SVN_ERR(svn_branch_branch_subtree(NULL,
                *from_subtree,
                tgt->branch, eid,
                scratch_pool));
    }
  else if (subbr_tgt)  /* added on target branch */
    {
      /* nothing to do */
    }
  else if (subbr_yca)  /* double delete */
    {
      /* ### conflict? policy option? */
    }

  return SVN_NO_ERROR;
}

/* Merge ...
 *
 * Merge any sub-branches in the same way, recursively.
 */
static svn_error_t *
branch_merge_subtree_r(svn_editor3_t *editor,
                       const svn_branch_el_rev_id_t *src,
                       const svn_branch_el_rev_id_t *tgt,
                       const svn_branch_el_rev_id_t *yca,
                       apr_pool_t *scratch_pool)
{
  svn_branch_subtree_t *s_src, *s_tgt, *s_yca;
  apr_hash_t *diff_yca_src, *diff_yca_tgt;
  svn_boolean_t had_conflict = FALSE;
  SVN_ITER_T(svn_branch_el_rev_content_t *) *pi;
  apr_hash_t *all_elements;
  const merge_conflict_policy_t policy = { TRUE, TRUE, TRUE, TRUE, TRUE };
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  SVN_ERR_ASSERT(src->eid == tgt->eid);
  SVN_ERR_ASSERT(src->eid == yca->eid);

  SVN_DBG(("merge src: r%2ld b%s e%3d",
           src->rev,
           svn_branch_get_id(src->branch, scratch_pool), src->eid));
  SVN_DBG(("merge tgt: r%2ld b%s e%3d",
           tgt->rev,
           svn_branch_get_id(tgt->branch, scratch_pool), tgt->eid));
  SVN_DBG(("merge yca: r%2ld b%s e%3d",
           yca->rev,
           svn_branch_get_id(yca->branch, scratch_pool), yca->eid));

  notify("merging into branch %s",
         svn_branch_get_id(tgt->branch, scratch_pool));
  /*
      for (eid, diff1) in element_differences(YCA, FROM):
        diff2 = element_diff(eid, YCA, TO)
        if diff1 and diff2:
          result := element_merge(diff1, diff2)
        elif diff1:
          result := diff1.right
        # else no change
   */
  s_src = svn_branch_get_subtree(src->branch, src->eid, scratch_pool);
  s_tgt = svn_branch_get_subtree(tgt->branch, tgt->eid, scratch_pool);
  s_yca = svn_branch_get_subtree(yca->branch, yca->eid, scratch_pool);
  SVN_ERR(svn_branch_subtree_differences(&diff_yca_src,
                                         editor, s_yca, s_src,
                                         scratch_pool, scratch_pool));
  /* ### We only need to query for YCA:TO differences in elements that are
         different in YCA:FROM, but right now we ask for all differences. */
  SVN_ERR(svn_branch_subtree_differences(&diff_yca_tgt,
                                         editor, s_yca, s_tgt,
                                         scratch_pool, scratch_pool));

  all_elements = apr_hash_overlay(scratch_pool,
                                  svn_branch_get_elements(src->branch),
                                  svn_branch_get_elements(tgt->branch));
  all_elements = apr_hash_overlay(scratch_pool,
                                  svn_branch_get_elements(yca->branch),
                                  all_elements);
  for (SVN_HASH_ITER_SORTED(pi, all_elements,
                            sort_compare_items_by_eid, scratch_pool))
    {
      int eid = *(const int *)(pi->key);
      svn_branch_el_rev_content_t **e_yca_src
        = svn_int_hash_get(diff_yca_src, eid);
      svn_branch_el_rev_content_t **e_yca_tgt
        = svn_int_hash_get(diff_yca_tgt, eid);
      svn_branch_el_rev_content_t *e_yca;
      svn_branch_el_rev_content_t *e_src;
      svn_branch_el_rev_content_t *e_tgt;
      svn_branch_el_rev_content_t *result;
      svn_boolean_t conflict;

      svn_pool_clear(iterpool);

      /* If an element hasn't changed in the source branch, there is
         no need to do anything with it in the target branch. We could
         use element_merge() for any case where at least one of (SRC,
         TGT, YCA) exists, but we choose to skip it when SRC == YCA. */
      if (! e_yca_src)
        {
          /* Still need to merge subbranch */
          SVN_ERR(merge_subbranch(editor, src, tgt, yca, eid, iterpool));

          continue;
        }

      e_yca = e_yca_src[0];
      e_src = e_yca_src[1];
      e_tgt = e_yca_tgt ? e_yca_tgt[1] : e_yca_src[0];

      element_merge(&result, &conflict,
                    eid, e_src, e_tgt, e_yca,
                    &policy,
                    scratch_pool, scratch_pool);

      if (conflict)
        {
          notify("!    e%d <conflict>", eid);
          had_conflict = TRUE;
        }
      else if (e_tgt && result)
        {
          notify("M/V  e%d %s%s",
                 eid, result->name,
                 subbranch_str(tgt->branch, eid, iterpool));

          SVN_ERR(svn_editor3_alter(editor, tgt->branch, eid,
                                    result->parent_eid, result->name,
                                    result->payload));

          SVN_ERR(merge_subbranch(editor, src, tgt, yca, eid, iterpool));
        }
      else if (e_tgt)
        {
          notify("D    e%d %s%s",
                 eid, e_yca->name,
                 subbranch_str(yca->branch, eid, iterpool));
          SVN_ERR(svn_editor3_delete(editor, tgt->branch, eid));
        }
      else if (result)
        {
          notify("A    e%d %s%s",
                 eid, result->name,
                 subbranch_str(src->branch, eid, iterpool));

          /* In BRANCH, create an instance of the element EID with new content.
           *
           * Translated to old language, this means create a new node-copy
           * copied (branched) from the source-right version of the merge
           * (which is not specified here, but will need to be),
           * which may be in this branch or in another branch.
           */
          SVN_ERR(svn_editor3_alter(editor, tgt->branch, eid,
                                    result->parent_eid, result->name,
                                    result->payload));

          SVN_ERR(merge_subbranch(editor, src, tgt, yca, eid, iterpool));
        }
    }
  svn_pool_destroy(iterpool);

  notify("merging into branch %s -- finished",
         svn_branch_get_id(tgt->branch, scratch_pool));

  if (had_conflict)
    {
      return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                               _("Merge failed: conflict(s) occurred"));
    }
  else
    {
      SVN_DBG(("merge completed: no conflicts"));
    }

  return SVN_NO_ERROR;
}

/* Merge SRC into TGT, using the common ancestor YCA.
 *
 * Merge the two sets of changes: YCA -> SRC and YCA -> TGT, applying
 * the result to the transaction at TGT.
 *
 * If conflicts arise, just fail.
 *
 * SRC, TGT and YCA must be existing and corresponding (same EID) elements.
 *
 * None of SRC, TGT and YCA is a subbranch root element.
 *
 * Nested subbranches will also be merged.
 */
static svn_error_t *
svn_branch_merge(svn_editor3_t *editor,
                 svn_branch_el_rev_id_t *src,
                 svn_branch_el_rev_id_t *tgt,
                 svn_branch_el_rev_id_t *yca,
                 apr_pool_t *scratch_pool)
{
  /*SVN_ERR(verify_exists_in_branch(from, scratch_pool));*/
  /*SVN_ERR(verify_exists_in_branch(to, scratch_pool));*/
  /*SVN_ERR(verify_exists_in_branch(yca, scratch_pool));*/
  if (src->eid != tgt->eid || src->eid != yca->eid)
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                             _("Merge branches must all be same element "
                               "(from: e%d, to: e%d, yca: e%d)"),
                             src->eid, tgt->eid, yca->eid);
  /*SVN_ERR(verify_not_subbranch_root(from, scratch_pool));*/
  /*SVN_ERR(verify_not_subbranch_root(to, scratch_pool));*/
  /*SVN_ERR(verify_not_subbranch_root(yca, scratch_pool));*/

  SVN_ERR(branch_merge_subtree_r(editor, src, tgt, yca, scratch_pool));

  return SVN_NO_ERROR;
}

/* Update the WC to revision BASE_REVISION (SVN_INVALID_REVNUM means HEAD).
 *
 * Merge any changes in the existing txn into the new txn.
 */
static svn_error_t *
do_update(svnmover_wc_t *wc,
          svn_revnum_t revision,
          apr_pool_t *scratch_pool)
{
  /* Keep hold of the previous WC txn */
  svn_branch_revision_root_t *previous_base_state
    = svn_array_get(wc->edit_txn->repos->rev_roots, (int)wc->base_revision);
  svn_branch_revision_root_t *previous_working_txn = wc->edit_txn;
  svn_branch_el_rev_id_t *yca, *src, *tgt;

  /* Complete the old edit drive into the 'WC' txn */
  SVN_ERR(svn_editor3_complete(wc->editor));

  /* Check out a new WC, re-using the same data object */
  SVN_ERR(wc_checkout(wc, revision,
                      scratch_pool));

  /* Merge changes from the old into the new WC */
  yca = svn_branch_el_rev_id_create(previous_base_state->root_branch,
                                    previous_base_state->root_branch->root_eid,
                                    previous_base_state->rev, scratch_pool);
  src = svn_branch_el_rev_id_create(previous_working_txn->root_branch,
                                    previous_working_txn->root_branch->root_eid,
                                    previous_working_txn->rev, scratch_pool);
  tgt = svn_branch_el_rev_id_create(wc->edit_txn->root_branch,
                                    wc->edit_txn->root_branch->root_eid,
                                    wc->edit_txn->rev, scratch_pool);
  SVN_ERR(svn_branch_merge(wc->editor, src, tgt, yca,
                           scratch_pool));
  /* ### TODO: If the merge raises conflicts, either revert to the
         pre-update state or store and handle the conflicts. Currently
         this just leaves the merge partially done and raises an error. */

  return SVN_NO_ERROR;
}

/*  */
typedef struct diff_item_t
{
  int eid;
  svn_branch_el_rev_content_t *e0, *e1;
  const char *relpath0, *relpath1;
  svn_boolean_t modified, reparented, renamed;
} diff_item_t;

/* Return differences between branch subtrees S_LEFT and S_RIGHT.
 *
 * Set *DIFF_CHANGES to an array of (diff_item_t).
 *
 * ### This requires 'subtrees' only in order to produce the 'relpath'
 *     fields in the output. Other than that, it would work with arbitrary
 *     sets of elements.
 */
static svn_error_t *
subtree_diff(svn_array_t **diff_changes,
             svn_editor3_t *editor,
             svn_branch_subtree_t *s_left,
             svn_branch_subtree_t *s_right,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  apr_hash_t *diff_left_right;
  apr_hash_index_t *hi;

  *diff_changes = svn_array_make(result_pool);

  SVN_ERR(svn_branch_subtree_differences(&diff_left_right,
                                         editor, s_left, s_right,
                                         result_pool, scratch_pool));

  for (hi = apr_hash_first(scratch_pool, diff_left_right);
       hi; hi = apr_hash_next(hi))
    {
      int eid = svn_int_hash_this_key(hi);
      svn_branch_el_rev_content_t **e_pair = apr_hash_this_val(hi);
      svn_branch_el_rev_content_t *e0 = e_pair[0], *e1 = e_pair[1];

      if (e0 || e1)
        {
          diff_item_t *item = apr_palloc(result_pool, sizeof(*item));

          item->eid = eid;
          item->e0 = e0;
          item->e1 = e1;
          item->relpath0 = e0 ? svn_branch_subtree_get_path_by_eid(
                                  s_left, eid, result_pool) : NULL;
          item->relpath1 = e1 ? svn_branch_subtree_get_path_by_eid(
                                  s_right, eid, result_pool) : NULL;
          item->reparented = (e0 && e1 && e0->parent_eid != e1->parent_eid);
          item->renamed = (e0 && e1 && strcmp(e0->name, e1->name) != 0);

          SVN_ARRAY_PUSH(*diff_changes) = item;
        }
    }

  return SVN_NO_ERROR;
}

/*  */
static int
diff_ordering(const void *a, const void *b)
{
  const diff_item_t *item_a = *(void *const *)a, *item_b = *(void *const *)b;
  int deleted_a = (item_a->e0 && ! item_a->e1);
  int deleted_b = (item_b->e0 && ! item_b->e1);
  const char *major_path_a = (item_a->e1 ? item_a->relpath1 : item_a->relpath0);
  const char *major_path_b = (item_b->e1 ? item_b->relpath1 : item_b->relpath0);

  /* Sort deleted items before all others */
  if (deleted_a != deleted_b)
    return deleted_b - deleted_a;

  /* Sort by path */
  return svn_path_compare_paths(major_path_a, major_path_b);
}

/*  */
static int
diff_ordering_eids(const void *a, const void *b)
{
  const diff_item_t *item_a = *(void *const *)a, *item_b = *(void *const *)b;

  return (item_a->eid < item_b->eid) ? -1 : (item_a->eid > item_b->eid) ? 1 : 0;
}

/* Display differences between flat branches LEFT and RIGHT.
 *
 * The output refers to paths or to elements according to THE_UI_MODE.
 */
static svn_error_t *
flat_branch_diff(svn_editor3_t *editor,
                 flat_branch_t *left,
                 flat_branch_t *right,
                 const char *prefix,
                 const char *header,
                 int eid_width,
                 apr_pool_t *scratch_pool)
{
  svn_array_t *diff_changes;
  SVN_ITER_T(diff_item_t) *ai;

  SVN_ERR_ASSERT(left && flat_branch_get_root_eid(left) >= 0
                 && right && flat_branch_get_root_eid(right) >= 0);

  SVN_ERR(subtree_diff(&diff_changes, editor, left->s, right->s,
                       scratch_pool, scratch_pool));

  if (header && diff_changes->nelts)
    printf("%s%s", prefix, header);

  for (SVN_ARRAY_ITER_SORTED(ai, diff_changes,
                             (the_ui_mode == UI_MODE_EIDS)
                               ? diff_ordering_eids : diff_ordering,
                             scratch_pool))
    {
      diff_item_t *item = ai->val;
      svn_branch_el_rev_content_t *e0 = item->e0, *e1 = item->e1;
      char status_mod = (e0 && e1) ? 'M' : e0 ? 'D' : 'A';

      if (the_ui_mode == UI_MODE_PATHS)
        {
          const char *major_path = (e1 ? item->relpath1 : item->relpath0);
          const char *from = "";

          if (item->reparented || item->renamed)
            {
              if (! item->reparented)
                from = apr_psprintf(scratch_pool,
                                    " (renamed from .../%s)",
                                    e0->name);
              else if (! item->renamed)
                from = apr_psprintf(scratch_pool,
                                    " (moved from %s/...)",
                                    svn_relpath_dirname(item->relpath0,
                                                        scratch_pool));
              else
                from = apr_psprintf(scratch_pool,
                                    " (moved+renamed from %s)",
                                    item->relpath0);
            }
          printf("%s%c%c%c %s%s%s\n",
                 prefix,
                 status_mod,
                 item->reparented ? 'v' : ' ', item->renamed ? 'r' : ' ',
                 major_path,
                 flat_branch_subbranch_str(e0 ? left : right,
                                       item->eid, scratch_pool),
                 from);
        }
      else
        {
          printf("%s%c%c%c e%-*d  %s%s%s%s\n",
                 prefix,
                 status_mod,
                 item->reparented ? 'v' : ' ', item->renamed ? 'r' : ' ',
                 eid_width, item->eid,
                 e1 ? apr_psprintf(scratch_pool, "e%-*d/%s",
                                   eid_width, e1->parent_eid, e1->name) : "",
                 flat_branch_subbranch_str(e0 ? left : right,
                                       item->eid, scratch_pool),
                 e0 && e1 ? " from " : "",
                 e0 ? apr_psprintf(scratch_pool, "e%-*d/%s",
                                   eid_width, e0->parent_eid, e0->name) : "");
        }
    }

  return SVN_NO_ERROR;
}

typedef svn_error_t *
svn_branch_diff_func_t(svn_editor3_t *editor,
                       flat_branch_t *left,
                       flat_branch_t *right,
                       const char *prefix,
                       const char *header,
                       int eid_width,
                       apr_pool_t *scratch_pool);

/* Display differences between flat branches LEFT and RIGHT.
 *
 * Recurse into sub-branches.
 */
static svn_error_t *
flat_branch_diff_r(svn_editor3_t *editor,
                   flat_branch_t *fb_left,
                   flat_branch_t *fb_right,
                   svn_branch_diff_func_t diff_func,
                   const char *prefix,
                   apr_pool_t *scratch_pool)
{
  const char *left_str = flat_branch_id_and_path(fb_left, scratch_pool);
  const char *right_str = flat_branch_id_and_path(fb_right, scratch_pool);
  const char *header;
  apr_hash_t *subbranches_l, *subbranches_r, *subbranches_all;
  apr_hash_index_t *hi;

  SVN_DBG(("flat_branch_diff_r: l='%s' r='%s'",
           fb_left ? fb_left->rrpath : "<nil>",
           fb_right ? fb_right->rrpath : "<nil>"));

  if (!fb_left)
    {
      header = apr_psprintf(scratch_pool,
                 "--- added branch %s\n",
                 right_str);
      printf("%s%s", prefix, header);
    }
  else if (!fb_right)
    {
      header = apr_psprintf(scratch_pool,
                 "--- deleted branch %s\n",
                 left_str);
      printf("%s%s", prefix, header);
    }
  else
    {
      if (strcmp(left_str, right_str) == 0)
        {
          header = apr_psprintf(
                     scratch_pool, "--- diff branch %s\n",
                     left_str);
        }
      else
        {
          header = apr_psprintf(
                     scratch_pool, "--- diff branch %s : %s\n",
                     left_str, right_str);
        }
      SVN_ERR(diff_func(editor, fb_left, fb_right, prefix, header, 3 /*eid_width*/,
                        scratch_pool));
    }

  /* recurse into each subbranch that exists in LEFT and/or in RIGHT */
  subbranches_l = fb_left ? flat_branch_get_subbranches(fb_left, scratch_pool)
                          : apr_hash_make(scratch_pool);
  subbranches_r = fb_right ? flat_branch_get_subbranches(fb_right, scratch_pool)
                           : apr_hash_make(scratch_pool);
  subbranches_all = apr_hash_overlay(scratch_pool,
                                     subbranches_l, subbranches_r);

  for (hi = apr_hash_first(scratch_pool, subbranches_all);
       hi; hi = apr_hash_next(hi))
    {
      int e = svn_int_hash_this_key(hi);
      flat_branch_t *sub_fb_left = NULL, *sub_fb_right = NULL;

      /* recurse */
      if (fb_left)
        sub_fb_left = flat_branch_get_subbranch_at_eid(fb_left, e,
                                                       scratch_pool);
      if (fb_right)
        sub_fb_right = flat_branch_get_subbranch_at_eid(fb_right, e,
                                                        scratch_pool);
      SVN_ERR(flat_branch_diff_r(editor,
                                 sub_fb_left, sub_fb_right,
                                 diff_func, prefix, scratch_pool));
    }
  return SVN_NO_ERROR;
}

/* Display differences between branch subtrees LEFT and RIGHT.
 *
 * Recurse into sub-branches.
 */
static svn_error_t *
svn_branch_diff_r(svn_editor3_t *editor,
                  svn_branch_el_rev_id_t *left,
                  svn_branch_el_rev_id_t *right,
                  svn_branch_diff_func_t diff_func,
                  const char *prefix,
                  apr_pool_t *scratch_pool)
{
  flat_branch_t *fb_left
    = branch_get_flat_branch(left->branch, left->eid, scratch_pool);
  flat_branch_t *fb_right
    = branch_get_flat_branch(right->branch, right->eid, scratch_pool);

  SVN_ERR(flat_branch_diff_r(editor,
                             fb_left, fb_right,
                             diff_func, prefix, scratch_pool));
  return SVN_NO_ERROR;
}

/* Move by branch-and-delete into an existing target branch.
 *
 * The target branch is different from the source branch.
 *
 *      delete elements from source branch
 *      instantiate (or update) same elements in target branch
 *
 * For each element being moved, if the element already exists in TO_BRANCH,
 * the effect is as if the existing element in TO_BRANCH was first deleted.
 */
static svn_error_t *
move_by_branch_and_delete(svn_editor3_t *editor,
                          svn_branch_el_rev_id_t *el_rev,
                          svn_branch_state_t *to_branch,
                          int to_parent_eid,
                          const char *to_name,
                          apr_pool_t *scratch_pool)
{
  svn_branch_subtree_t *subtree
    = svn_branch_get_subtree(el_rev->branch, el_rev->eid, scratch_pool);

  /* This is supposed to be used for moving to a *different* branch.
     In fact, this method would also work for moving within one
     branch, but we don't currently want to use it for that purpose. */
  SVN_ERR_ASSERT(! BRANCH_IS_SAME_BRANCH(el_rev->branch, to_branch,
                                         scratch_pool));

  /* Delete the source subtree. If it's a whole branch, do so by deleting
     its root from the outer branch instead. */
  if (el_rev->eid != el_rev->branch->root_eid)
    {
      SVN_ERR(svn_editor3_delete(editor,
                                 el_rev->branch, el_rev->eid));
    }
  else
    {
      SVN_ERR(svn_editor3_delete(editor,
                                 el_rev->branch->outer_branch, el_rev->branch->outer_eid));
    }
  SVN_ERR(svn_branch_instantiate_subtree(to_branch,
                                         to_parent_eid, to_name, *subtree,
                                         scratch_pool));

  return SVN_NO_ERROR;
}

/* Move by copy-and-delete.
 *
 * The target branch is different from the source branch.
 *
 *      copy source elements to target branch
 *      delete elements from source branch
 *
 * For each element being moved, if the element already exists in TO_BRANCH,
 * the effect is as if the existing element in TO_BRANCH was first deleted.
 */
static svn_error_t *
move_by_copy_and_delete(svn_editor3_t *editor,
                        svn_branch_el_rev_id_t *el_rev,
                        svn_branch_state_t *to_branch,
                        int to_parent_eid,
                        const char *to_name,
                        apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_editor3_copy_tree(editor, el_rev,
                                to_branch,
                                to_parent_eid, to_name));
  SVN_ERR(svn_editor3_delete(editor,
                             el_rev->branch, el_rev->eid));
  return SVN_NO_ERROR;
}

/* Move in the 'best' way possible.
 *
 *    if target is in same branch:
 *      move the element
 *    else [target is in another branch]:
 *      delete from source branch
 *      instantiate in target branch
 *    [else:
 *      copy into target branch
 *      delete from source branch]
 */
static svn_error_t *
do_move(svn_editor3_t *editor,
        svn_branch_el_rev_id_t *el_rev,
        svn_branch_el_rev_id_t *to_parent_el_rev,
        const char *to_name,
        apr_pool_t *scratch_pool)
{
  /* Simple move/rename within same branch, if possible */
  if (BRANCH_IS_SAME_BRANCH(to_parent_el_rev->branch, el_rev->branch,
                            scratch_pool))
    {
      /* New payload shall be the same as before */
      svn_branch_el_rev_content_t *existing_element
        = svn_branch_get_element(el_rev->branch, el_rev->eid);

      SVN_ERR(svn_editor3_alter(editor,
                                el_rev->branch, el_rev->eid,
                                to_parent_el_rev->eid, to_name,
                                existing_element->payload));
      return SVN_NO_ERROR;
    }

  /* Instantiate same elements in another branch */
  if (1 /*###*/)
    {
      /* Here the elements moved from the source branch will overwrite any
         corresponding elements that already exist in the target branch.
         We could instead check and either throw an error or fall back to
         copy-and-delete in that case. */

      printf("mv: moving by branch-and-delete\n");

      SVN_ERR(move_by_branch_and_delete(editor, el_rev,
                                        to_parent_el_rev->branch,
                                        to_parent_el_rev->eid, to_name,
                                        scratch_pool));
      return SVN_NO_ERROR;
    }

  /* Move by copy-and-delete */
  printf("mv: moving by copy-and-delete\n");

  SVN_ERR(move_by_copy_and_delete(editor, el_rev,
                                  to_parent_el_rev->branch,
                                  to_parent_el_rev->eid, to_name,
                                  scratch_pool));

  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
svn_branch_find_predecessor_el_rev(svn_branch_el_rev_id_t **new_el_rev_p,
                                   svn_branch_el_rev_id_t *old_el_rev,
                                   apr_pool_t *result_pool)
{
  const svn_branch_repos_t *repos = old_el_rev->branch->rev_root->repos;
  const char *branch_id;

  if (old_el_rev->rev <= 0)
    return NULL;

  branch_id = svn_branch_get_id(old_el_rev->branch, result_pool);
  SVN_ERR(svn_branch_repos_find_el_rev_by_id(new_el_rev_p,
                                             repos, old_el_rev->rev - 1,
                                             branch_id, old_el_rev->eid,
                                             result_pool, result_pool));
  return SVN_NO_ERROR;
}

/* Similar to 'svn log -v', this iterates over the revisions between
 * LEFT and RIGHT (currently excluding LEFT), printing a single-rev diff
 * for each.
 */
static svn_error_t *
svn_branch_log(svn_editor3_t *editor,
               svn_branch_el_rev_id_t *left,
               svn_branch_el_rev_id_t *right,
               apr_pool_t *scratch_pool)
{
  svn_revnum_t first_rev = left->rev, rev;

  for (rev = right->rev; rev > first_rev; rev--)
    {
      svn_branch_el_rev_id_t *el_rev_left;

      SVN_ERR(svn_branch_find_predecessor_el_rev(&el_rev_left, right, scratch_pool));

      printf(SVN_CL__LOG_SEP_STRING "r%ld | ...\n",
             rev);
      printf("Changed elements:\n");
      SVN_ERR(svn_branch_diff_r(editor,
                                el_rev_left, right,
                                flat_branch_diff, "   ",
                                scratch_pool));
      right = el_rev_left;
    }

  return SVN_NO_ERROR;
}

/* Make a subbranch at OUTER_BRANCH : OUTER_PARENT_EID : OUTER_NAME.
 *
 * The subbranch will consist of a single element given by PAYLOAD.
 */
static svn_error_t *
mk_branch(svn_branch_state_t **new_branch_p,
          svn_editor3_t *editor,
          svn_branch_state_t *outer_branch,
          int outer_parent_eid,
          const char *outer_name,
          svn_element_payload_t *payload,
          apr_pool_t *iterpool)
{
  int new_outer_eid;
  svn_branch_state_t *new_branch;

  SVN_ERR(svn_editor3_new_eid(editor, &new_outer_eid, outer_branch));
  SVN_ERR(svn_editor3_add(editor,
                          outer_branch, new_outer_eid,
                          outer_parent_eid, outer_name,
                          NULL /*new_payload*/));
  new_branch = svn_branch_add_new_branch(
                 outer_branch, new_outer_eid, -1/*new_root_eid*/,
                 iterpool);
  svn_branch_update_element(new_branch, new_branch->root_eid,
                            -1, "", payload);
  *new_branch_p = new_branch;
  return SVN_NO_ERROR;
}

/* This commit callback prints not only a commit summary line but also
 * a log-style summary of the changes.
 */
static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool)
{
  commit_callback_baton_t *b = baton;

  SVN_ERR(svn_cmdline_printf(pool, "Committed r%ld:\n",
                             commit_info->revision));

  b->revision = commit_info->revision;
  return SVN_NO_ERROR;
}

/* Display a diff of the commit */
static svn_error_t *
display_diff_of_commit(const commit_callback_baton_t *ccbb,
                       apr_pool_t *scratch_pool)
{
  svn_branch_el_rev_id_t *el_rev_left, *el_rev_right;
  const char *rrpath = "";
  const svn_branch_repos_t *repos = ccbb->edit_txn->repos;

  SVN_ERR(svn_branch_repos_find_el_rev_by_path_rev(&el_rev_left,
                                                   rrpath,
                                                   ccbb->revision - 1,
                                                   repos, scratch_pool,
                                                   scratch_pool));
  SVN_ERR(svn_branch_repos_find_el_rev_by_path_rev(&el_rev_right,
                                                   rrpath,
                                                   ccbb->revision,
                                                   repos, scratch_pool,
                                                   scratch_pool));
  SVN_ERR(svn_branch_diff_r(ccbb->editor,
                            el_rev_left, el_rev_right,
                            flat_branch_diff, "   ",
                            scratch_pool));
  return SVN_NO_ERROR;
}

/* Commit and update WC.
 *
 * Set *NEW_REV_P to the committed revision number, and update the WC to
 * that revision.
 *
 * If there are no changes to commit, set *NEW_REV_P to SVN_INVALID_REVNUM
 * and do not make a commit and do not update the WC.
 *
 * NEW_REV_P may be null if not wanted.
 */
static svn_error_t *
do_commit(svn_revnum_t *new_rev_p,
          svnmover_wc_t *wc,
          apr_hash_t *revprops,
          apr_pool_t *scratch_pool)
{
  svn_revnum_t new_rev;

  /* Complete the old edit drive (into the 'WC') */
  SVN_ERR(svn_editor3_complete(wc->editor));

  /* Commit */
  SVN_ERR(wc_commit(&new_rev, wc, revprops, scratch_pool));

  /* Check out a new WC if a commit was performed */
  if (SVN_IS_VALID_REVNUM(new_rev))
    {
      SVN_ERR(wc_checkout(wc, new_rev, scratch_pool));
    }

  if (new_rev_p)
    *new_rev_p = new_rev;
  return SVN_NO_ERROR;
}

/* Revert all uncommitted changes in WC.
 */
static svn_error_t *
do_revert(svnmover_wc_t *wc,
          apr_pool_t *scratch_pool)
{
  svn_branch_revision_root_t *base_txn
    = svn_array_get(wc->edit_txn->repos->rev_roots, (int)wc->base_revision);

  /* Replay the inverse of the current edit txn, into the current edit txn */
  SVN_ERR(replay(wc->editor, wc->edit_txn->root_branch,
                 wc->edit_txn,
                 base_txn,
                 scratch_pool));

  return SVN_NO_ERROR;
}

typedef struct arg_t
{
  const char *path_name;
  svn_revnum_t revnum;
  svn_branch_el_rev_id_t *el_rev, *parent_el_rev;
} arg_t;

#define VERIFY_REV_SPECIFIED(op, i)                                     \
  if (arg[i]->el_rev->rev == SVN_INVALID_REVNUM)                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: '%s': revision number required"),   \
                             op, action->relpath[i]);

#define VERIFY_REV_UNSPECIFIED(op, i)                                   \
  if (arg[i]->el_rev->rev != SVN_INVALID_REVNUM)                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: '%s@...': revision number not allowed"), \
                             op, action->relpath[i]);

#define VERIFY_EID_NONEXISTENT(op, i)                                   \
  if (arg[i]->el_rev->eid != -1)                                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Element already exists at path '%s'"), \
                             op, action->relpath[i]);

#define VERIFY_EID_EXISTS(op, i)                                        \
  if (arg[i]->el_rev->eid == -1)                                        \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Element not found at path '%s%s'"), \
                             op, action->relpath[i],                    \
                             action->rev_spec[i].kind == svn_opt_revision_unspecified \
                               ? "" : "@...");

#define VERIFY_PARENT_EID_EXISTS(op, i)                                 \
  if (arg[i]->parent_el_rev->eid == -1)                                 \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL,                   \
                             _("%s: Element not found at path '%s'"),   \
                             op, svn_relpath_dirname(action->relpath[i], pool));

#define is_branch_root_element(branch, eid) \
  ((branch)->root_eid == (eid))

/* If EL_REV is the root element of a branch, return the corresponding
 * subbranch-root element of its outer branch.
 *
 * If it is the repository root, return null.
 *
 * Otherwise, return itself.
 */
static svn_branch_el_rev_id_t *
point_to_outer_element_instead(svn_branch_el_rev_id_t *el_rev,
                               apr_pool_t *result_pool)
{
  svn_branch_el_rev_id_t *new_el_rev = el_rev;

  if (is_branch_root_element(el_rev->branch, el_rev->eid))
    {
      if (! el_rev->branch->outer_branch)
        return NULL;

      new_el_rev = svn_branch_el_rev_id_create(el_rev->branch->outer_branch,
                                               el_rev->branch->outer_eid,
                                               el_rev->rev,
                                               result_pool);
    }

  return new_el_rev;
}

static svn_error_t *
execute(svnmover_wc_t *wc,
        const apr_array_header_t *actions,
        const char *anchor_url,
        apr_hash_t *revprops,
        svn_client_ctx_t *ctx,
        apr_pool_t *pool)
{
  svn_editor3_t *editor;
  const char *base_relpath;
  apr_pool_t *iterpool = svn_pool_create(pool);
  int i;

  editor = wc->editor;
  base_relpath = svn_uri_skip_ancestor(wc->repos_root_url, anchor_url, pool);

  for (i = 0; i < actions->nelts; ++i)
    {
      action_t *action = APR_ARRAY_IDX(actions, i, action_t *);
      int j;
      arg_t *arg[3] = { NULL, NULL, NULL };

      svn_pool_clear(iterpool);

      /* Before translating paths to/from elements, need a sequence point */
      svn_editor3_sequence_point(editor);

      /* Convert each ACTION[j].{relpath, rev_spec} to
         (EL_REV[j], PARENT_EL_REV[j], PATH_NAME[j], REVNUM[j]),
         except for the local-path argument of a 'put' command. */
      for (j = 0; j < 3; j++)
        {
          if (action->relpath[j]
              && ! (action->action == ACTION_PUT_FILE && j == 0))
            {
              const char *rrpath, *parent_rrpath;

              arg[j] = apr_palloc(iterpool, sizeof(*arg[j]));
              if (action->rev_spec[j].kind == svn_opt_revision_unspecified)
                arg[j]->revnum = SVN_INVALID_REVNUM;
              else if (action->rev_spec[j].kind == svn_opt_revision_number)
                arg[j]->revnum = action->rev_spec[j].value.number;
              else if (action->rev_spec[j].kind == svn_opt_revision_head)
                {
                  arg[j]->revnum = wc->head_revision;
                }
              else if (action->rev_spec[j].kind == svn_opt_revision_base
                       || action->rev_spec[j].kind == svn_opt_revision_committed)
                {
                  arg[j]->revnum = wc->base_revision;
                }
              else
                return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                         "'%s@...': revision specifier "
                                         "must be a number or 'head', 'base' "
                                         "or 'committed'",
                                         action->relpath[j]);

              rrpath = svn_relpath_join(base_relpath, action->relpath[j], iterpool);
              parent_rrpath = svn_relpath_dirname(rrpath, iterpool);

              arg[j]->path_name = svn_relpath_basename(rrpath, NULL);
              SVN_ERR(find_el_rev_by_rrpath_rev(&arg[j]->el_rev, wc,
                                                arg[j]->revnum, rrpath,
                                                iterpool, iterpool));
              SVN_ERR(find_el_rev_by_rrpath_rev(&arg[j]->parent_el_rev, wc,
                                                arg[j]->revnum, parent_rrpath,
                                                iterpool, iterpool));
            }
        }

      switch (action->action)
        {
        case ACTION_DIFF:
          VERIFY_EID_EXISTS("diff", 0);
          VERIFY_EID_EXISTS("diff", 1);
          {
            SVN_ERR(svn_branch_diff_r(editor,
                                      arg[0]->el_rev /*from*/,
                                      arg[1]->el_rev /*to*/,
                                      flat_branch_diff, "",
                                      iterpool));
          }
          break;

        case ACTION_STATUS:
          {
            svn_branch_el_rev_id_t *from, *to;

            SVN_ERR(find_el_rev_by_rrpath_rev(&from, wc,
                                              wc->base_revision, base_relpath,
                                              iterpool, iterpool));
            SVN_ERR(find_el_rev_by_rrpath_rev(&to, wc,
                                              SVN_INVALID_REVNUM, base_relpath,
                                              iterpool, iterpool));
            SVN_ERR(svn_branch_diff_r(editor,
                                      from, to,
                                      flat_branch_diff, "",
                                      iterpool));
          }
          break;

        case ACTION_LOG:
          VERIFY_EID_EXISTS("log", 0);
          VERIFY_EID_EXISTS("log", 1);
          {
            SVN_ERR(svn_branch_log(editor,
                                   arg[0]->el_rev /*from*/,
                                   arg[1]->el_rev /*to*/,
                                   iterpool));
          }
          break;

        case ACTION_LIST_BRANCHES:
          {
            VERIFY_EID_EXISTS("branches", 0);
            if (the_ui_mode == UI_MODE_PATHS)
              {
                printf("branches rooted at same element as '%s':\n",
                       action->relpath[0]);
              }
            else
              {
                printf("branches rooted at e%d:\n", arg[0]->el_rev->eid);
              }
            SVN_ERR(list_branches(
                      arg[0]->el_rev->branch->rev_root,
                      arg[0]->el_rev->eid,
                      FALSE, iterpool));
          }
          break;

        case ACTION_LIST_BRANCHES_R:
          {
            /* (Note: BASE_REVISION is always a real revision number, here) */
            SVN_ERR(list_all_branches(wc->edit_txn, TRUE, iterpool));
          }
          break;

        case ACTION_LS:
          {
            VERIFY_EID_EXISTS("ls", 0);
            if (the_ui_mode == UI_MODE_PATHS)
              {
                flat_branch_t *fb
                  = branch_get_flat_branch(
                      arg[0]->el_rev->branch, arg[0]->el_rev->branch->root_eid, iterpool);
                SVN_ERR(list_branch_elements(fb, iterpool));
              }
            else
              SVN_ERR(list_branch_elements_by_eid(arg[0]->el_rev->branch, iterpool));
          }
          break;

        case ACTION_BRANCH:
          VERIFY_EID_EXISTS("branch", 0);
          VERIFY_REV_UNSPECIFIED("branch", 1);
          VERIFY_EID_NONEXISTENT("branch", 1);
          VERIFY_PARENT_EID_EXISTS("branch", 1);
          {
            svn_branch_state_t *new_branch;

            SVN_ERR(svn_branch_branch(&new_branch,
                                      arg[0]->el_rev->branch, arg[0]->el_rev->eid,
                                      arg[1]->el_rev->branch, arg[1]->parent_el_rev->eid,
                                      arg[1]->path_name,
                                      iterpool));
            notify("A+   %s%s", action->relpath[1],
                   branch_str(new_branch, iterpool));
          }
          break;

        case ACTION_BRANCH_INTO:
          VERIFY_EID_EXISTS("branch-into", 0);
          VERIFY_REV_UNSPECIFIED("branch-into", 1);
          VERIFY_EID_NONEXISTENT("branch-into", 1);
          VERIFY_PARENT_EID_EXISTS("branch-into", 1);
          {
            SVN_ERR(svn_branch_branch_into(arg[0]->el_rev->branch, arg[0]->el_rev->eid,
                                           arg[1]->el_rev->branch,
                                           arg[1]->parent_el_rev->eid, arg[1]->path_name,
                                           iterpool));
            notify("A+   %s (subtree)", action->relpath[1]);
          }
          break;

        case ACTION_MKBRANCH:
          VERIFY_REV_UNSPECIFIED("mkbranch", 0);
          VERIFY_EID_NONEXISTENT("mkbranch", 0);
          VERIFY_PARENT_EID_EXISTS("mkbranch", 0);
          {
            apr_hash_t *props = apr_hash_make(iterpool);
            svn_element_payload_t *payload
              = svn_element_payload_create_dir(props, iterpool);
            svn_branch_state_t *new_branch;

            SVN_ERR(mk_branch(&new_branch,
                              editor, arg[0]->parent_el_rev->branch,
                              arg[0]->parent_el_rev->eid, arg[0]->path_name,
                              payload, iterpool));
            notify("A    %s%s", action->relpath[0],
                   branch_str(new_branch, iterpool));
          }
          break;

        case ACTION_MERGE:
          {
            VERIFY_EID_EXISTS("merge", 0);
            VERIFY_EID_EXISTS("merge", 1);
            VERIFY_EID_EXISTS("merge", 2);
            SVN_ERR(svn_branch_merge(editor,
                                     arg[0]->el_rev /*from*/,
                                     arg[1]->el_rev /*to*/,
                                     arg[2]->el_rev /*yca*/,
                                     iterpool));
          }
          break;

        case ACTION_MV:
          /* If given a branch root element, look instead at the
             subbranch-root element within the outer branch. */
          arg[0]->el_rev = point_to_outer_element_instead(arg[0]->el_rev, iterpool);
          if (! arg[0]->el_rev)
            return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                                     _("mv: cannot move the repository root"));

          if (svn_relpath_skip_ancestor(action->relpath[0], action->relpath[1]))
            return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                                     _("mv: cannot move to child of self"));
          VERIFY_REV_UNSPECIFIED("mv", 0);
          VERIFY_EID_EXISTS("mv", 0);
          VERIFY_REV_UNSPECIFIED("mv", 1);
          VERIFY_EID_NONEXISTENT("mv", 1);
          VERIFY_PARENT_EID_EXISTS("mv", 1);
          SVN_ERR(do_move(editor, arg[0]->el_rev, arg[1]->parent_el_rev, arg[1]->path_name,
                          iterpool));
          notify("V    %s (from %s)", action->relpath[1], action->relpath[0]);
          break;

        case ACTION_CP:
          VERIFY_REV_SPECIFIED("cp", 0);
            /* (Or do we want to support copying from "this txn" too?) */
          VERIFY_EID_EXISTS("cp", 0);
          VERIFY_REV_UNSPECIFIED("cp", 1);
          VERIFY_EID_NONEXISTENT("cp", 1);
          VERIFY_PARENT_EID_EXISTS("cp", 1);
          SVN_ERR(svn_editor3_copy_tree(editor,
                                        arg[0]->el_rev,
                                        arg[1]->parent_el_rev->branch,
                                        arg[1]->parent_el_rev->eid, arg[1]->path_name));
          notify("A+   %s (from %s)", action->relpath[1], action->relpath[0]);
          break;

        case ACTION_RM:
          /* If given a branch root element, look instead at the
             subbranch-root element within the outer branch. */
          arg[0]->el_rev = point_to_outer_element_instead(arg[0]->el_rev, iterpool);
          if (! arg[0]->el_rev)
            return svn_error_createf(SVN_ERR_BRANCHING, NULL,
                                     _("rm: cannot remove the repository root"));

          VERIFY_REV_UNSPECIFIED("rm", 0);
          VERIFY_EID_EXISTS("rm", 0);
          SVN_ERR(svn_editor3_delete(editor,
                                     arg[0]->el_rev->branch, arg[0]->el_rev->eid));
          notify("D    %s", action->relpath[0]);
          break;

        case ACTION_MKDIR:
          VERIFY_REV_UNSPECIFIED("mkdir", 0);
          VERIFY_EID_NONEXISTENT("mkdir", 0);
          VERIFY_PARENT_EID_EXISTS("mkdir", 0);
          {
            apr_hash_t *props = apr_hash_make(iterpool);
            svn_element_payload_t *payload
              = svn_element_payload_create_dir(props, iterpool);
            int new_eid;

            SVN_ERR(svn_editor3_new_eid(editor, &new_eid,
                                        arg[0]->parent_el_rev->branch));
            SVN_ERR(svn_editor3_add(editor,
                                    arg[0]->parent_el_rev->branch, new_eid,
                                    arg[0]->parent_el_rev->eid, arg[0]->path_name,
                                    payload));
          }
          notify("A    %s", action->relpath[0]);
          break;

        case ACTION_PUT_FILE:
          VERIFY_REV_UNSPECIFIED("put", 1);
          VERIFY_PARENT_EID_EXISTS("put", 1);
          {
            apr_hash_t *props = apr_hash_make(iterpool);
            svn_stringbuf_t *text;
            int parent_eid;
            const char *name;
            svn_element_payload_t *payload;

            if (arg[1]->el_rev->eid >= 0)
              {
                /* ### get existing props */
                props = apr_hash_make(iterpool);
              }
            else
              {
                props = apr_hash_make(iterpool);
              }
            /* read new text from file */
            {
              svn_stream_t *src;

              if (strcmp(action->relpath[0], "-") != 0)
                SVN_ERR(svn_stream_open_readonly(&src, action->relpath[0],
                                                 pool, iterpool));
              else
                SVN_ERR(svn_stream_for_stdin(&src, pool));

              svn_stringbuf_from_stream(&text, src, 0, iterpool);
            }
            payload = svn_element_payload_create_file(props, text, iterpool);

            if (is_branch_root_element(arg[1]->el_rev->branch,
                                       arg[1]->el_rev->eid))
              {
                parent_eid = -1;
                name = "";
              }
            else
              {
                parent_eid = arg[1]->parent_el_rev->eid;
                name = arg[1]->path_name;
              }

            if (arg[1]->el_rev->eid >= 0)
              {
                SVN_ERR(svn_editor3_alter(editor,
                                          arg[1]->el_rev->branch, arg[1]->el_rev->eid,
                                          parent_eid, name,
                                          payload));
              }
            else
              {
                int new_eid;

                SVN_ERR(svn_editor3_new_eid(editor, &new_eid,
                                            arg[1]->parent_el_rev->branch));
                SVN_ERR(svn_editor3_add(editor,
                                        arg[1]->parent_el_rev->branch, new_eid,
                                        parent_eid, name,
                                        payload));
              }
          }
          notify("A    %s", action->relpath[1]);
          break;

        case ACTION_COMMIT:
          {
            svn_revnum_t new_rev;

            SVN_ERR(do_commit(&new_rev, wc, revprops, iterpool));
            if (SVN_IS_VALID_REVNUM(new_rev))
              {
                editor = wc->editor;
              }
            else
              {
                printf("There are no changes to commit.\n");
              }
          }
          break;

        case ACTION_UPDATE:
          /* path (or eid) is currently required for syntax, but ignored */
          VERIFY_EID_EXISTS("update", 0);
          VERIFY_REV_SPECIFIED("update", 0);
          {
              SVN_ERR(do_update(wc, arg[0]->revnum, iterpool));
              editor = wc->editor;
          }
          break;

        case ACTION_REVERT:
          {
            SVN_ERR(do_revert(wc, iterpool));
          }
          break;

        default:
          SVN_ERR_MALFUNCTION();
        }
    }
  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/*  */
static svn_error_t *
final_commit(svnmover_wc_t *wc,
             apr_hash_t *revprops,
             apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  /* Complete the old edit drive (into the 'WC') */
  SVN_ERR(svn_editor3_complete(wc->editor));

  /* Commit, if there are any changes */
  err = wc_commit(NULL, wc, revprops, scratch_pool);

  svn_pool_destroy(wc->pool);

  return svn_error_trace(err);
}

/* Perform the typical suite of manipulations for user-provided URLs
   on URL, returning the result (allocated from POOL): IRI-to-URI
   conversion, auto-escaping, and canonicalization. */
static const char *
sanitize_url(const char *url,
             apr_pool_t *pool)
{
  url = svn_path_uri_from_iri(url, pool);
  url = svn_path_uri_autoescape(url, pool);
  return svn_uri_canonicalize(url, pool);
}

static const char *
help_for_subcommand(const action_defn_t *action, apr_pool_t *pool)
{
  const char *cmd = apr_psprintf(pool, "%s %s",
                                 action->name, action->args_help);

  return apr_psprintf(pool, "  %-22s : %s\n", cmd, action->help);
}

/* Print a usage message on STREAM, listing only the actions. */
static void
usage_actions_only(FILE *stream, apr_pool_t *pool)
{
  int i;

  for (i = 0; i < sizeof (action_defn) / sizeof (action_defn[0]); i++)
    svn_error_clear(svn_cmdline_fputs(
                      help_for_subcommand(&action_defn[i], pool),
                      stream, pool));
}

/* Print a usage message on STREAM. */
static void
usage(FILE *stream, apr_pool_t *pool)
{
  svn_error_clear(svn_cmdline_fputs(
    _("usage: svnmover -U REPO_URL [ACTION...]\n"
      "A client for experimenting with move tracking.\n"
      "\n"
      "  Perform URL-based ACTIONs on a Subversion repository, committing the\n"
      "  result as a (single) new revision, similar to svnmucc.\n"
      "\n"
      "  With no ACTIONs, read actions interactively from standard input, making\n"
      "  one commit for each line of input.\n"
      "\n"
      "  Store move tracking metadata either in local files or in revprops.\n"
      "\n"
      "Actions:\n"),
                  stream, pool));
  usage_actions_only(stream, pool);
  svn_error_clear(svn_cmdline_fputs(
    _("\n"
      "Valid options:\n"
      "  --ui={eids|e|paths|p}  : display information as elements or as paths\n"
      "  -h, -? [--help]        : display this text\n"
      "  -v [--verbose]         : display debugging messages\n"
      "  -q [--quiet]           : suppress notifications\n"
      "  -m [--message] ARG     : use ARG as a log message\n"
      "  -F [--file] ARG        : read log message from file ARG\n"
      "  -u [--username] ARG    : commit the changes as username ARG\n"
      "  -p [--password] ARG    : use ARG as the password\n"
      "  -U [--root-url] ARG    : interpret all action URLs relative to ARG\n"
      "  -r [--revision] ARG    : use revision ARG as baseline for changes\n"
      "  --with-revprop ARG     : set revision property in the following format:\n"
      "                               NAME[=VALUE]\n"
      "  --non-interactive      : do no interactive prompting (default is to\n"
      "                           prompt only if standard input is a terminal)\n"
      "  --force-interactive    : do interactive prompting even if standard\n"
      "                           input is not a terminal\n"
      "  --trust-server-cert    : accept SSL server certificates from unknown\n"
      "                           certificate authorities without prompting (but\n"
      "                           only with '--non-interactive')\n"
      "  -X [--extra-args] ARG  : append arguments from file ARG (one per line;\n"
      "                           use \"-\" to read from standard input)\n"
      "  --config-dir ARG       : use ARG to override the config directory\n"
      "  --config-option ARG    : use ARG to override a configuration option\n"
      "  --no-auth-cache        : do not cache authentication tokens\n"
      "  --version              : print version information\n"),
                  stream, pool));
}

static svn_error_t *
insufficient(int i, apr_pool_t *pool)
{
  return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                           "insufficient arguments:\n"
                           "%s",
                           help_for_subcommand(&action_defn[i], pool));
}

static svn_error_t *
display_version(apr_getopt_t *os, svn_boolean_t _quiet, apr_pool_t *pool)
{
  const char *ra_desc_start
    = "The following repository access (RA) modules are available:\n\n";
  svn_stringbuf_t *version_footer;

  version_footer = svn_stringbuf_create(ra_desc_start, pool);
  SVN_ERR(svn_ra_print_modules(version_footer, pool));

  SVN_ERR(svn_opt_print_help4(NULL, "svnmover", TRUE, _quiet, FALSE,
                              version_footer->data,
                              NULL, NULL, NULL, NULL, NULL, pool));

  return SVN_NO_ERROR;
}

/* Return an error about the mutual exclusivity of the -m, -F, and
   --with-revprop=svn:log command-line options. */
static svn_error_t *
mutually_exclusive_logs_error(void)
{
  return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                          _("--message (-m), --file (-F), and "
                            "--with-revprop=svn:log are mutually "
                            "exclusive"));
}

/* Obtain the log message from multiple sources, producing an error
   if there are multiple sources. Store the result in *FINAL_MESSAGE.  */
static svn_error_t *
sanitize_log_sources(const char **final_message,
                     const char *message,
                     apr_hash_t *revprops,
                     svn_stringbuf_t *filedata,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_string_t *msg;

  *final_message = NULL;
  /* If we already have a log message in the revprop hash, then just
     make sure the user didn't try to also use -m or -F.  Otherwise,
     we need to consult -m or -F to find a log message, if any. */
  msg = svn_hash_gets(revprops, SVN_PROP_REVISION_LOG);
  if (msg)
    {
      if (filedata || message)
        return mutually_exclusive_logs_error();

      *final_message = apr_pstrdup(result_pool, msg->data);

      /* Will be re-added by libsvn_client */
      svn_hash_sets(revprops, SVN_PROP_REVISION_LOG, NULL);
    }
  else if (filedata)
    {
      if (message)
        return mutually_exclusive_logs_error();

      *final_message = apr_pstrdup(result_pool, filedata->data);
    }
  else if (message)
    {
      *final_message = apr_pstrdup(result_pool, message);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
log_message_func(const char **log_msg,
                 svn_boolean_t non_interactive,
                 const char *log_message,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  if (log_message)
    {
      svn_string_t *message = svn_string_create(log_message, pool);

      SVN_ERR_W(svn_subst_translate_string2(&message, NULL, NULL,
                                            message, NULL, FALSE,
                                            pool, pool),
                _("Error normalizing log message to internal format"));

      *log_msg = message->data;

      return SVN_NO_ERROR;
    }

  if (non_interactive)
    {
      return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, NULL,
                              _("Cannot invoke editor to get log message "
                                "when non-interactive"));
    }
  else
    {
      svn_string_t *msg = svn_string_create("", pool);

      SVN_ERR(svn_cmdline__edit_string_externally(
                      &msg, NULL, NULL, "", msg, "svnmover-commit",
                      ctx->config, TRUE, NULL, pool));

      if (msg && msg->data)
        *log_msg = msg->data;
      else
        *log_msg = NULL;

      return SVN_NO_ERROR;
    }
}

/* Parse the action arguments into action structures. */
static svn_error_t *
parse_actions(apr_array_header_t **actions,
              apr_array_header_t *action_args,
              apr_pool_t *pool)
{
  int i;

  *actions = apr_array_make(pool, 1, sizeof(action_t *));

  for (i = 0; i < action_args->nelts; ++i)
    {
      int j, k, num_url_args;
      const char *action_string = APR_ARRAY_IDX(action_args, i, const char *);
      action_t *action = apr_pcalloc(pool, sizeof(*action));
      const char *cp_from_rev = NULL;

      /* First, parse the action. */
      if (! strcmp(action_string, "?") || ! strcmp(action_string, "h")
          || ! strcmp(action_string, "help"))
        {
          usage_actions_only(stdout, pool);
          return SVN_NO_ERROR;
        }
      for (j = 0; j < sizeof(action_defn) / sizeof(action_defn[0]); j++)
        {
          if (strcmp(action_string, action_defn[j].name) == 0)
            {
              action->action = action_defn[j].code;
              num_url_args = action_defn[j].num_args;
              break;
            }
        }
      if (j == sizeof(action_defn) / sizeof(action_defn[0]))
        return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                 "'%s' is not an action",
                                 action_string);

      if (action->action == ACTION_CP)
        {
          /* next argument is the copy source revision */
          if (++i == action_args->nelts)
            return svn_error_trace(insufficient(j, pool));
          cp_from_rev = APR_ARRAY_IDX(action_args, i, const char *);
        }

      /* Parse the required number of URLs. */
      for (k = 0; k < num_url_args; ++k)
        {
          const char *path;

          if (++i == action_args->nelts)
            return svn_error_trace(insufficient(j, pool));
          path = APR_ARRAY_IDX(action_args, i, const char *);

          if (cp_from_rev && k == 0)
            {
              path = apr_psprintf(pool, "%s@%s", path, cp_from_rev);
            }

          SVN_ERR(svn_opt_parse_path(&action->rev_spec[k], &path, path, pool));

          /* If there's an ANCHOR_URL, we expect URL to be a path
             relative to ANCHOR_URL (and we build a full url from the
             combination of the two).  Otherwise, it should be a full
             url. */
          if (svn_path_is_url(path))
            {
              return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                       "Argument '%s' is a URL; use "
                                       "--root-url (-U) instead", path);
            }
          /* These args must be relpaths, except for the 'local file' arg
             of a 'put' command. */
          if (! svn_relpath_is_canonical(path)
              && ! (action->action == ACTION_PUT_FILE && k == 0))
            {
              return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                       "Argument '%s' is not a relative path "
                                       "or a URL", path);
            }
          action->relpath[k] = path;
        }

      APR_ARRAY_PUSH(*actions, action_t *) = action;
    }

  return SVN_NO_ERROR;
}

/* A command-line completion callback for the 'Line Noise' interactive
 * prompting.
 *
 * This is called when the user presses the Tab key. It calculates the
 * possible completions for the partial line BUF.
 *
 * ### So far, this only works on a single command keyword at the start
 *     of the line.
 */
static void
linenoise_completion(const char *buf, linenoiseCompletions *lc)
{
  int i;

  for (i = 0; i < sizeof(action_defn) / sizeof(action_defn[0]); i++)
    {
      /* Suggest each command that matches (and is longer than) what the
         user has already typed. Add a space. */
      if (strncmp(buf, action_defn[i].name, strlen(buf)) == 0
          && strlen(action_defn[i].name) > strlen(buf))
        {
          static char completion[100];

          apr_cpystrn(completion, action_defn[i].name, 99);
          strcat(completion, " ");
          linenoiseAddCompletion(lc, completion);
        }
    }
}

/* Display a prompt, read a line of input and split it into words.
 *
 * Set *WORDS to null if input is cancelled (by ctrl-C for example).
 */
static svn_error_t *
read_words(apr_array_header_t **words,
           const char *prompt,
           apr_pool_t *result_pool)
{
  svn_error_t *err;
  const char *input;

  err = svn_cmdline_prompt_user2(&input, prompt, NULL, result_pool);
  if (err && (err->apr_err == SVN_ERR_CANCELLED || err->apr_err == APR_EOF))
    {
      *words = NULL;
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  SVN_ERR(err);
  *words = svn_cstring_split(input, " ", TRUE /*chop_whitespace*/, result_pool);

  return SVN_NO_ERROR;
}

/*
 * On success, leave *EXIT_CODE untouched and return SVN_NO_ERROR. On error,
 * either return an error to be displayed, or set *EXIT_CODE to non-zero and
 * return SVN_NO_ERROR.
 */
static svn_error_t *
sub_main(int *exit_code, int argc, const char *argv[], apr_pool_t *pool)
{
  apr_array_header_t *actions;
  svn_error_t *err = SVN_NO_ERROR;
  apr_getopt_t *opts;
  enum {
    config_dir_opt = SVN_OPT_FIRST_LONGOPT_ID,
    config_inline_opt,
    no_auth_cache_opt,
    version_opt,
    with_revprop_opt,
    non_interactive_opt,
    force_interactive_opt,
    trust_server_cert_opt,
    ui_opt
  };
  static const apr_getopt_option_t options[] = {
    {"verbose", 'v', 0, ""},
    {"quiet", 'q', 0, ""},
    {"branch", 'b', 1, ""},
    {"message", 'm', 1, ""},
    {"file", 'F', 1, ""},
    {"username", 'u', 1, ""},
    {"password", 'p', 1, ""},
    {"root-url", 'U', 1, ""},
    {"revision", 'r', 1, ""},
    {"with-revprop",  with_revprop_opt, 1, ""},
    {"extra-args", 'X', 1, ""},
    {"help", 'h', 0, ""},
    {NULL, '?', 0, ""},
    {"non-interactive", non_interactive_opt, 0, ""},
    {"force-interactive", force_interactive_opt, 0, ""},
    {"trust-server-cert", trust_server_cert_opt, 0, ""},
    {"config-dir", config_dir_opt, 1, ""},
    {"config-option",  config_inline_opt, 1, ""},
    {"no-auth-cache",  no_auth_cache_opt, 0, ""},
    {"version", version_opt, 0, ""},
    {"ui", ui_opt, 1, ""},
    {NULL, 0, 0, NULL}
  };
  const char *message = "";
  svn_stringbuf_t *filedata = NULL;
  const char *username = NULL, *password = NULL;
  const char *anchor_url = NULL, *extra_args_file = NULL;
  const char *config_dir = NULL;
  apr_array_header_t *config_options;
  svn_boolean_t show_version = FALSE;
  svn_boolean_t non_interactive = FALSE;
  svn_boolean_t force_interactive = FALSE;
  svn_boolean_t interactive_actions;
  svn_boolean_t trust_server_cert = FALSE;
  svn_boolean_t no_auth_cache = FALSE;
  svn_revnum_t base_revision = SVN_INVALID_REVNUM;
  apr_array_header_t *action_args;
  apr_hash_t *revprops = apr_hash_make(pool);
  apr_hash_t *cfg_hash;
  svn_config_t *cfg_config;
  svn_client_ctx_t *ctx;
  const char *log_msg;
  svnmover_wc_t *wc;

  /* Check library versions */
  SVN_ERR(check_lib_versions());

  /* Suppress debug message unless '-v' given. */
  svn_dbg__set_quiet_mode(TRUE);

  config_options = apr_array_make(pool, 0,
                                  sizeof(svn_cmdline__config_argument_t*));

  apr_getopt_init(&opts, pool, argc, argv);
  opts->interleave = 1;
  while (1)
    {
      int opt;
      const char *arg;
      const char *opt_arg;

      apr_status_t status = apr_getopt_long(opts, options, &opt, &arg);
      if (APR_STATUS_IS_EOF(status))
        break;
      if (status != APR_SUCCESS)
        return svn_error_wrap_apr(status, "getopt failure");
      switch(opt)
        {
        case 'v':
          svn_dbg__set_quiet_mode(FALSE);
          break;
        case 'q':
          quiet = TRUE;
          break;
        case 'm':
          SVN_ERR(svn_utf_cstring_to_utf8(&message, arg, pool));
          break;
        case 'F':
          {
            const char *arg_utf8;
            SVN_ERR(svn_utf_cstring_to_utf8(&arg_utf8, arg, pool));
            SVN_ERR(svn_stringbuf_from_file2(&filedata, arg, pool));
          }
          break;
        case 'u':
          username = apr_pstrdup(pool, arg);
          break;
        case 'p':
          password = apr_pstrdup(pool, arg);
          break;
        case 'U':
          SVN_ERR(svn_utf_cstring_to_utf8(&anchor_url, arg, pool));
          if (! svn_path_is_url(anchor_url))
            return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                                     "'%s' is not a URL", anchor_url);
          anchor_url = sanitize_url(anchor_url, pool);
          break;
        case 'r':
          {
            const char *saved_arg = arg;
            char *digits_end = NULL;
            while (*arg == 'r')
              arg++;
            base_revision = strtol(arg, &digits_end, 10);
            if ((! SVN_IS_VALID_REVNUM(base_revision))
                || (! digits_end)
                || *digits_end)
              return svn_error_createf(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                       _("Invalid revision number '%s'"),
                                       saved_arg);
          }
          break;
        case with_revprop_opt:
          SVN_ERR(svn_opt_parse_revprop(&revprops, arg, pool));
          break;
        case 'X':
          extra_args_file = apr_pstrdup(pool, arg);
          break;
        case non_interactive_opt:
          non_interactive = TRUE;
          break;
        case force_interactive_opt:
          force_interactive = TRUE;
          break;
        case trust_server_cert_opt:
          trust_server_cert = TRUE;
          break;
        case config_dir_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&config_dir, arg, pool));
          break;
        case config_inline_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&opt_arg, arg, pool));
          SVN_ERR(svn_cmdline__parse_config_option(config_options, opt_arg,
                                                   "svnmover: ", pool));
          break;
        case no_auth_cache_opt:
          no_auth_cache = TRUE;
          break;
        case version_opt:
          show_version = TRUE;
          break;
        case ui_opt:
          SVN_ERR(svn_utf_cstring_to_utf8(&opt_arg, arg, pool));
          SVN_ERR(svn_token__from_word_err(&the_ui_mode, ui_mode_map, opt_arg));
          break;
        case 'h':
        case '?':
          usage(stdout, pool);
          return SVN_NO_ERROR;
        }
    }

  if (show_version)
    {
      SVN_ERR(display_version(opts, quiet, pool));
      return SVN_NO_ERROR;
    }

  if (non_interactive && force_interactive)
    {
      return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                              _("--non-interactive and --force-interactive "
                                "are mutually exclusive"));
    }
  else
    non_interactive = !svn_cmdline__be_interactive(non_interactive,
                                                   force_interactive);

  if (trust_server_cert && !non_interactive)
    {
      return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                              _("--trust-server-cert requires "
                                "--non-interactive"));
    }

  /* Now initialize the client context */

  err = svn_config_get_config(&cfg_hash, config_dir, pool);
  if (err)
    {
      /* Fallback to default config if the config directory isn't readable
         or is not a directory. */
      if (APR_STATUS_IS_EACCES(err->apr_err)
          || SVN__APR_STATUS_IS_ENOTDIR(err->apr_err))
        {
          svn_handle_warning2(stderr, err, "svnmover: ");
          svn_error_clear(err);

          SVN_ERR(svn_config__get_default_config(&cfg_hash, pool));
        }
      else
        return err;
    }

  if (config_options)
    {
      svn_error_clear(
          svn_cmdline__apply_config_options(cfg_hash, config_options,
                                            "svnmover: ", "--config-option"));
    }

  SVN_ERR(svn_client_create_context2(&ctx, cfg_hash, pool));

  cfg_config = svn_hash_gets(cfg_hash, SVN_CONFIG_CATEGORY_CONFIG);
  SVN_ERR(svn_cmdline_create_auth_baton(&ctx->auth_baton,
                                        non_interactive,
                                        username,
                                        password,
                                        config_dir,
                                        no_auth_cache,
                                        trust_server_cert,
                                        cfg_config,
                                        ctx->cancel_func,
                                        ctx->cancel_baton,
                                        pool));

  /* Make sure we have a log message to use. */
  SVN_ERR(sanitize_log_sources(&log_msg, message, revprops, filedata,
                               pool, pool));

  /* Get the commit log message */
  SVN_ERR(log_message_func(&log_msg, non_interactive, log_msg, ctx, pool));
  if (! log_msg)
    return SVN_NO_ERROR;

  /* Put the log message in the list of revprops, and check that the user
     did not try to supply any other "svn:*" revprops. */
  if (svn_prop_has_svn_prop(revprops, pool))
    return svn_error_create(SVN_ERR_CLIENT_PROPERTY_NAME, NULL,
                            _("Standard properties can't be set "
                              "explicitly as revision properties"));
  svn_hash_sets(revprops, SVN_PROP_REVISION_LOG,
                svn_string_create(log_msg, pool));

  /* Help command: if given before any actions, then display full help
     (and ANCHOR_URL need not have been provided). */
  if (opts->ind < opts->argc && strcmp(opts->argv[opts->ind], "help") == 0)
    {
      usage(stdout, pool);
      return SVN_NO_ERROR;
    }

  if (!anchor_url)
    return svn_error_createf(SVN_ERR_INCORRECT_PARAMS, NULL,
                             "--root-url (-U) not provided");

  /* Copy the rest of our command-line arguments to an array,
     UTF-8-ing them along the way. */
  /* If there are extra arguments in a supplementary file, tack those
     on, too (again, in UTF8 form). */
  action_args = apr_array_make(pool, opts->argc, sizeof(const char *));
  if (extra_args_file)
    {
      const char *extra_args_file_utf8;
      svn_stringbuf_t *contents, *contents_utf8;

      SVN_ERR(svn_utf_cstring_to_utf8(&extra_args_file_utf8,
                                      extra_args_file, pool));
      SVN_ERR(svn_stringbuf_from_file2(&contents, extra_args_file_utf8, pool));
      SVN_ERR(svn_utf_stringbuf_to_utf8(&contents_utf8, contents, pool));
      svn_cstring_split_append(action_args, contents_utf8->data, "\n\r",
                               FALSE, pool);
    }

  interactive_actions = !(opts->ind < opts->argc
                          || extra_args_file
                          || non_interactive);

  if (interactive_actions)
    {
      linenoiseSetCompletionCallback(linenoise_completion);
    }

  SVN_ERR(wc_create(&wc,
                    anchor_url, base_revision,
                    ctx, pool, pool));
  do
    {
      /* Parse arguments -- converting local style to internal style,
       * repos-relative URLs to regular URLs, etc. */
      err = svn_client_args_to_target_array2(&action_args, opts, action_args,
                                             ctx, FALSE, pool);
      if (! err)
        err = parse_actions(&actions, action_args, pool);
      if (! err)
        err = execute(wc, actions, anchor_url, revprops, ctx, pool);
      if (err)
        {
          if (err->apr_err == SVN_ERR_AUTHN_FAILED && non_interactive)
            err = svn_error_quick_wrap(err,
                                       _("Authentication failed and interactive"
                                         " prompting is disabled; see the"
                                         " --force-interactive option"));
          if (interactive_actions)
            {
              /* Display the error, but don't quit */
              svn_handle_error2(err, stderr, FALSE, "svnmover: ");
              svn_error_clear(err);
            }
          else
            SVN_ERR(err);
        }

      /* Possibly read more actions from the command line */
      if (interactive_actions)
        {
          SVN_ERR(read_words(&action_args, "svnmover> ", pool));
        }
    }
  while (interactive_actions && action_args);

  SVN_ERR(final_commit(wc, revprops, pool));

  return SVN_NO_ERROR;
}

int
main(int argc, const char *argv[])
{
  apr_pool_t *pool;
  int exit_code = EXIT_SUCCESS;
  svn_error_t *err;

  /* Initialize the app. */
  if (svn_cmdline_init("svnmover", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  /* Create our top-level pool.  Use a separate mutexless allocator,
   * given this application is single threaded.
   */
  pool = apr_allocator_owner_get(svn_pool_create_allocator(FALSE));

  svn_error_set_malfunction_handler(svn_error_raise_on_malfunction);

  err = sub_main(&exit_code, argc, argv, pool);

  /* Flush stdout and report if it fails. It would be flushed on exit anyway
     but this makes sure that output is not silently lost if it fails. */
  err = svn_error_compose_create(err, svn_cmdline_fflush(stdout));

  if (err)
    {
      exit_code = EXIT_FAILURE;
      svn_cmdline_handle_exit_error(err, NULL, "svnmover: ");
    }

  svn_pool_destroy(pool);
  return exit_code;
}