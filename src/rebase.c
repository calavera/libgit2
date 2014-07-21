/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "buffer.h"
#include "repository.h"
#include "posix.h"
#include "filebuf.h"
#include "merge.h"
#include "array.h"
#include "config.h"

#include <git2/types.h>
#include <git2/rebase.h>
#include <git2/commit.h>
#include <git2/reset.h>
#include <git2/revwalk.h>
#include <git2/notes.h>

#define REBASE_APPLY_DIR	"rebase-apply"
#define REBASE_MERGE_DIR	"rebase-merge"

#define HEAD_NAME_FILE		"head-name"
#define ORIG_HEAD_FILE		"orig-head"
#define HEAD_FILE			"head"
#define ONTO_FILE			"onto"
#define ONTO_NAME_FILE		"onto_name"
#define QUIET_FILE			"quiet"

#define MSGNUM_FILE			"msgnum"
#define END_FILE			"end"
#define CMT_FILE_FMT		"cmt.%d"
#define CURRENT_FILE		"current"
#define REWRITTEN_FILE		"rewritten"

#define ORIG_DETACHED_HEAD	"detached HEAD"

#define NOTES_DEFAULT_REF	NULL

#define REBASE_DIR_MODE		0777
#define REBASE_FILE_MODE	0666

typedef enum {
	GIT_REBASE_TYPE_NONE = 0,
	GIT_REBASE_TYPE_APPLY = 1,
	GIT_REBASE_TYPE_MERGE = 2,
	GIT_REBASE_TYPE_INTERACTIVE = 3,
} git_rebase_type_t;

struct git_rebase_state_merge {
	int32_t msgnum;
	int32_t end;
	char *onto_name;

	git_commit *current;
};

typedef struct {
	git_rebase_type_t type;
	char *state_path;

	int head_detached : 1;

	char *orig_head_name;
	git_oid orig_head_id;

	git_oid onto_id;

	union {
		struct git_rebase_state_merge merge;
	};
} git_rebase_state;

#define GIT_REBASE_STATE_INIT {0}

static int rebase_state_type(
	git_rebase_type_t *type_out,
	char **path_out,
	git_repository *repo)
{
	git_buf path = GIT_BUF_INIT;
	git_rebase_type_t type = GIT_REBASE_TYPE_NONE;

	if (git_buf_joinpath(&path, repo->path_repository, REBASE_APPLY_DIR) < 0)
		return -1;

	if (git_path_isdir(git_buf_cstr(&path))) {
		type = GIT_REBASE_TYPE_APPLY;
		goto done;
	}

	git_buf_clear(&path);
	if (git_buf_joinpath(&path, repo->path_repository, REBASE_MERGE_DIR) < 0)
		return -1;

	if (git_path_isdir(git_buf_cstr(&path))) {
		type = GIT_REBASE_TYPE_MERGE;
		goto done;
	}

done:
	*type_out = type;

	if (type != GIT_REBASE_TYPE_NONE && path_out)
		*path_out = git_buf_detach(&path);

	git_buf_free(&path);

	return 0;
}

static int rebase_state_merge(git_rebase_state *state, git_repository *repo)
{
	git_buf path = GIT_BUF_INIT, msgnum = GIT_BUF_INIT, end = GIT_BUF_INIT,
		onto_name = GIT_BUF_INIT, current = GIT_BUF_INIT;
	git_oid current_id;
	int state_path_len, error;

	GIT_UNUSED(repo);

	if ((error = git_buf_puts(&path, state->state_path)) < 0)
		goto done;

	state_path_len = git_buf_len(&path);

	/* Read 'end' */
	if ((error = git_buf_joinpath(&path, path.ptr, END_FILE)) < 0 ||
		(error = git_futils_readbuffer(&end, path.ptr)) < 0)
		goto done;

	git_buf_rtrim(&end);

	if ((error = git__strtol32(&state->merge.end, end.ptr, NULL, 10)) < 0)
		goto done;

	/* Read 'onto_name' */
	git_buf_truncate(&path, state_path_len);

	if ((error = git_buf_joinpath(&path, path.ptr, ONTO_NAME_FILE)) < 0 ||
		(error = git_futils_readbuffer(&onto_name, path.ptr)) < 0)
		goto done;

	git_buf_rtrim(&onto_name);

	state->merge.onto_name = git_buf_detach(&onto_name);

	/* Read 'msgnum' if it exists, otherwise let msgnum = 0 */
	git_buf_truncate(&path, state_path_len);

	if ((error = git_buf_joinpath(&path, path.ptr, MSGNUM_FILE)) < 0)
		goto done;

	if (git_path_exists(path.ptr)) {
		if ((error = git_futils_readbuffer(&msgnum, path.ptr)) < 0)
			goto done;

		git_buf_rtrim(&msgnum);

		if ((error = git__strtol32(&state->merge.msgnum, msgnum.ptr, NULL, 10)) < 0)
			goto done;
	}


	/* Read 'current' if it exists, otherwise let current = null */
	git_buf_truncate(&path, state_path_len);

	if ((error = git_buf_joinpath(&path, path.ptr, CURRENT_FILE)) < 0)
		goto done;

	if (git_path_exists(path.ptr)) {
		if ((error = git_futils_readbuffer(&current, path.ptr)) < 0)
			goto done;

		git_buf_rtrim(&current);

		if ((error = git_oid_fromstr(&current_id, current.ptr)) < 0 ||
			(error = git_commit_lookup(&state->merge.current, repo, &current_id)) < 0)
			goto done;
	}

done:
	git_buf_free(&path);
	git_buf_free(&msgnum);
	git_buf_free(&end);
	git_buf_free(&onto_name);
	git_buf_free(&current);

	return error;
}

static int rebase_state(git_rebase_state *state, git_repository *repo)
{
	git_buf path = GIT_BUF_INIT, orig_head_name = GIT_BUF_INIT,
		orig_head_id = GIT_BUF_INIT, onto_id = GIT_BUF_INIT;
	int state_path_len, error;

	memset(state, 0x0, sizeof(git_rebase_state));

	if ((error = rebase_state_type(&state->type, &state->state_path, repo)) < 0)
		goto done;

	if (state->type == GIT_REBASE_TYPE_NONE) {
		giterr_set(GITERR_REBASE, "There is no rebase in progress");
		return GIT_ENOTFOUND;
	}

	if ((error = git_buf_puts(&path, state->state_path)) < 0)
		goto done;

	state_path_len = git_buf_len(&path);

	if ((error = git_buf_joinpath(&path, path.ptr, HEAD_NAME_FILE)) < 0 ||
		(error = git_futils_readbuffer(&orig_head_name, path.ptr)) < 0)
		goto done;

	git_buf_rtrim(&orig_head_name);

	if (strcmp(ORIG_DETACHED_HEAD, orig_head_name.ptr) == 0)
		state->head_detached = 1;

	git_buf_truncate(&path, state_path_len);

	if ((error = git_buf_joinpath(&path, path.ptr, ORIG_HEAD_FILE)) < 0)
		goto done;

	if (!git_path_isfile(path.ptr)) {
		/* Previous versions of git.git used 'head' here; support that. */
		git_buf_truncate(&path, state_path_len);

		if ((error = git_buf_joinpath(&path, path.ptr, HEAD_FILE)) < 0)
			goto done;
	}

	if ((error = git_futils_readbuffer(&orig_head_id, path.ptr)) < 0)
		goto done;

	git_buf_rtrim(&orig_head_id);

	if ((error = git_oid_fromstr(&state->orig_head_id, orig_head_id.ptr)) < 0)
		goto done;

	git_buf_truncate(&path, state_path_len);

	if ((error = git_buf_joinpath(&path, path.ptr, ONTO_FILE)) < 0 ||
		(error = git_futils_readbuffer(&onto_id, path.ptr)) < 0)
		goto done;

	git_buf_rtrim(&onto_id);

	if ((error = git_oid_fromstr(&state->onto_id, onto_id.ptr)) < 0)
		goto done;

	if (!state->head_detached)
		state->orig_head_name = git_buf_detach(&orig_head_name);

	switch (state->type) {
	case GIT_REBASE_TYPE_INTERACTIVE:
		giterr_set(GITERR_REBASE, "Interactive rebase is not supported");
		error = -1;
		break;
	case GIT_REBASE_TYPE_MERGE:
		error = rebase_state_merge(state, repo);
		break;
	case GIT_REBASE_TYPE_APPLY:
		giterr_set(GITERR_REBASE, "Patch application rebase is not supported");
		error = -1;
		break;
	default:
		abort();
	}

done:
	git_buf_free(&path);
	git_buf_free(&orig_head_name);
	git_buf_free(&orig_head_id);
	git_buf_free(&onto_id);
	return error;
}

static void rebase_state_free(git_rebase_state *state)
{
	if (state == NULL)
		return;

	if (state->type == GIT_REBASE_TYPE_MERGE) {
		git__free(state->merge.onto_name);
		git_commit_free(state->merge.current);
	}

	git__free(state->orig_head_name);
	git__free(state->state_path);
}

static int rebase_cleanup(git_rebase_state *state)
{
	return git_path_isdir(state->state_path) ?
		git_futils_rmdir_r(state->state_path, NULL, GIT_RMDIR_REMOVE_FILES) :
		0;
}

static int rebase_setupfile(git_repository *repo, const char *filename, int flags, const char *fmt, ...)
{
	git_buf path = GIT_BUF_INIT,
		contents = GIT_BUF_INIT;
	va_list ap;
	int error;

	va_start(ap, fmt);
	git_buf_vprintf(&contents, fmt, ap);
	va_end(ap);

	if ((error = git_buf_joinpath(&path, repo->path_repository, REBASE_MERGE_DIR)) == 0 &&
		(error = git_buf_joinpath(&path, path.ptr, filename)) == 0)
		error = git_futils_writebuffer(&contents, path.ptr, flags, REBASE_FILE_MODE);

	git_buf_free(&path);
	git_buf_free(&contents);

	return error;
}

/* TODO: git.git actually uses the literal argv here, this is an attempt
 * to emulate that.
 */
static const char *rebase_onto_name(const git_merge_head *onto)
{
	if (onto->ref_name && git__strncmp(onto->ref_name, "refs/heads/", 11) == 0)
		return onto->ref_name + 11;
	else if (onto->ref_name)
		return onto->ref_name;
	else
		return onto->oid_str;
}

static int rebase_setup_merge(
	git_repository *repo,
	const git_merge_head *branch,
	const git_merge_head *upstream,
	const git_merge_head *onto,
	const git_rebase_options *opts)
{
	git_revwalk *revwalk = NULL;
	git_commit *commit;
	git_buf commit_filename = GIT_BUF_INIT;
	git_oid id;
	char id_str[GIT_OID_HEXSZ];
	bool merge;
	int commit_cnt = 0, error;

	GIT_UNUSED(opts);

	if (!upstream)
		upstream = onto;

	if ((error = git_revwalk_new(&revwalk, repo)) < 0 ||
		(error = git_revwalk_push(revwalk, &branch->oid)) < 0 ||
		(error = git_revwalk_hide(revwalk, &upstream->oid)) < 0)
		goto done;

	git_revwalk_sorting(revwalk, GIT_SORT_REVERSE | GIT_SORT_TIME);

	while ((error = git_revwalk_next(&id, revwalk)) == 0) {
		if ((error = git_commit_lookup(&commit, repo, &id)) < 0)
			goto done;

		merge = (git_commit_parentcount(commit) > 1);
		git_commit_free(commit);

		if (merge)
			continue;

		commit_cnt++;

		git_buf_clear(&commit_filename);
		git_buf_printf(&commit_filename, CMT_FILE_FMT, commit_cnt);

		git_oid_fmt(id_str, &id);
		if ((error = rebase_setupfile(repo, commit_filename.ptr, -1,
				"%.*s\n", GIT_OID_HEXSZ, id_str)) < 0)
			goto done;
	}

	if (error != GIT_ITEROVER ||
		(error = rebase_setupfile(repo, END_FILE, -1, "%d\n", commit_cnt)) < 0)
		goto done;

	error = rebase_setupfile(repo, ONTO_NAME_FILE, -1, "%s\n",
		rebase_onto_name(onto));

done:
	git_revwalk_free(revwalk);
	git_buf_free(&commit_filename);

	return error;
}

static int rebase_setup(
	git_repository *repo,
	const git_merge_head *branch,
	const git_merge_head *upstream,
	const git_merge_head *onto,
	const git_rebase_options *opts)
{
	git_buf state_path = GIT_BUF_INIT;
	const char *orig_head_name;
	int error;

	if (git_buf_joinpath(&state_path, repo->path_repository, REBASE_MERGE_DIR) < 0)
		return -1;

	if ((error = p_mkdir(state_path.ptr, REBASE_DIR_MODE)) < 0) {
		giterr_set(GITERR_OS, "Failed to create rebase directory '%s'",
			state_path.ptr);
		goto done;
	}

	if ((error = git_repository__set_orig_head(repo, &branch->oid)) < 0)
		goto done;

	orig_head_name = branch->ref_name ? branch->ref_name : ORIG_DETACHED_HEAD;

	if ((error = rebase_setupfile(repo, HEAD_NAME_FILE, -1, "%s\n", orig_head_name)) < 0 ||
		(error = rebase_setupfile(repo, ONTO_FILE, -1, "%s\n", onto->oid_str)) < 0 ||
		(error = rebase_setupfile(repo, ORIG_HEAD_FILE, -1, "%s\n", branch->oid_str)) < 0 ||
		(error = rebase_setupfile(repo, QUIET_FILE, -1, opts->quiet ? "t\n" : "\n")) < 0)
		goto done;

	error = rebase_setup_merge(repo, branch, upstream, onto, opts);

done:
	if (error < 0)
		git_repository__cleanup_files(repo, (const char **)&state_path.ptr, 1);

	git_buf_free(&state_path);

	return error;
}

int git_rebase_init_options(git_rebase_options *opts, unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_rebase_options, GIT_REBASE_OPTIONS_INIT);
	return 0;
}

static int rebase_normalize_opts(
	git_repository *repo,
	git_rebase_options *opts,
	const git_rebase_options *given_opts)
{
	git_rebase_options default_opts = GIT_REBASE_OPTIONS_INIT;
	git_config *config;

	if (given_opts)
		memcpy(opts, given_opts, sizeof(git_rebase_options));
	else
		memcpy(opts, &default_opts, sizeof(git_rebase_options));

	if (git_repository_config(&config, repo) < 0)
		return -1;

	if (given_opts && given_opts->rewrite_notes_ref) {
		opts->rewrite_notes_ref = git__strdup(given_opts->rewrite_notes_ref);
		GITERR_CHECK_ALLOC(opts->rewrite_notes_ref);
	} else if (git_config__get_bool_force(config, "notes.rewrite.rebase", 1)) {
		const char *rewrite_ref = git_config__get_string_force(
			config, "notes.rewriteref", NOTES_DEFAULT_REF);

		if (rewrite_ref) {
			opts->rewrite_notes_ref = git__strdup(rewrite_ref);
			GITERR_CHECK_ALLOC(opts->rewrite_notes_ref);
		}
	}

	git_config_free(config);

	return 0;
}

static void rebase_opts_free(git_rebase_options *opts)
{
	if (!opts)
		return;

	git__free((char *)opts->rewrite_notes_ref);
}

static int rebase_ensure_not_in_progress(git_repository *repo)
{
	int error;
	git_rebase_type_t type;

	if ((error = rebase_state_type(&type, NULL, repo)) < 0)
		return error;

	if (type != GIT_REBASE_TYPE_NONE) {
		giterr_set(GITERR_REBASE, "There is an existing rebase in progress");
		return -1;
	}

	return 0;
}

static int rebase_ensure_not_dirty(git_repository *repo)
{
	git_tree *head = NULL;
	git_index *index = NULL;
	git_diff *diff = NULL;
	int error;

	if ((error = git_repository_head_tree(&head, repo)) < 0 ||
		(error = git_repository_index(&index, repo)) < 0 ||
		(error = git_diff_tree_to_index(&diff, repo, head, index, NULL)) < 0)
		goto done;

	if (git_diff_num_deltas(diff) > 0) {
		giterr_set(GITERR_REBASE, "Uncommitted changes exist in index");
		error = -1;
		goto done;
	}

	git_diff_free(diff);
	diff = NULL;

	if ((error = git_diff_index_to_workdir(&diff, repo, index, NULL)) < 0)
		goto done;

	if (git_diff_num_deltas(diff) > 0) {
		giterr_set(GITERR_REBASE, "Unstaged changes exist in workdir");
		error = -1;
	}

done:
	git_diff_free(diff);
	git_index_free(index);
	git_tree_free(head);

	return error;
}

int git_rebase(
	git_repository *repo,
	const git_merge_head *branch,
	const git_merge_head *upstream,
	const git_merge_head *onto,
	const git_signature *signature,
	const git_rebase_options *given_opts)
{
	git_rebase_options opts;
	git_reference *head_ref = NULL;
	git_buf reflog = GIT_BUF_INIT;
	git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
	int error;

	assert(repo && branch && (upstream || onto));

	GITERR_CHECK_VERSION(given_opts, GIT_MERGE_OPTIONS_VERSION, "git_merge_options");

	if ((error = rebase_normalize_opts(repo, &opts, given_opts)) < 0 ||
		(error = git_repository__ensure_not_bare(repo, "rebase")) < 0 ||
		(error = rebase_ensure_not_in_progress(repo)) < 0 ||
		(error = rebase_ensure_not_dirty(repo)) < 0)
		goto done;

	if (!onto)
		onto = upstream;

	if ((error = rebase_setup(repo, branch, upstream, onto, &opts)) < 0)
		goto done;

	if ((error = git_buf_printf(&reflog,
			"rebase: checkout %s", rebase_onto_name(onto))) < 0 ||
		(error = git_reference_create(&head_ref, repo, GIT_HEAD_FILE,
			&onto->oid, 1, signature, reflog.ptr)) < 0)
		goto done;

	checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE;
	error = git_checkout_head(repo, &checkout_opts);

done:
	git_reference_free(head_ref);
	git_buf_free(&reflog);
	rebase_opts_free(&opts);
	return error;
}

static int normalize_checkout_opts(
	git_repository *repo,
	git_checkout_options *checkout_opts,
	const git_checkout_options *given_checkout_opts,
	const git_rebase_state *state)
{
	int error = 0;

	GIT_UNUSED(repo);

	if (given_checkout_opts != NULL)
		memcpy(checkout_opts, given_checkout_opts, sizeof(git_checkout_options));
	else {
		git_checkout_options default_checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
		default_checkout_opts.checkout_strategy =  GIT_CHECKOUT_SAFE;

		memcpy(checkout_opts, &default_checkout_opts, sizeof(git_checkout_options));
	}

	if (!checkout_opts->ancestor_label)
		checkout_opts->ancestor_label = "ancestor";

	if (state->type == GIT_REBASE_TYPE_MERGE) {
		if (!checkout_opts->our_label)
			checkout_opts->our_label = state->merge.onto_name;

		if (!checkout_opts->their_label)
			checkout_opts->their_label = git_commit_summary(state->merge.current);
	} else {
		abort();
	}

	return error;
}

static int rebase_next_merge(
	git_repository *repo,
	git_rebase_state *state,
	git_checkout_options *given_checkout_opts)
{
	git_buf path = GIT_BUF_INIT, current = GIT_BUF_INIT;
	git_checkout_options checkout_opts = {0};
	git_oid current_id;
	git_commit *parent_commit = NULL;
	git_tree *current_tree = NULL, *head_tree = NULL, *parent_tree = NULL;
	git_index *index = NULL;
	unsigned int parent_count;
	int error;

	if (state->merge.msgnum == state->merge.end)
		return GIT_ITEROVER;

	state->merge.msgnum++;

	if ((error = git_buf_joinpath(&path, state->state_path, "cmt.")) < 0 ||
		(error = git_buf_printf(&path, "%d", state->merge.msgnum)) < 0 ||
		(error = git_futils_readbuffer(&current, path.ptr)) < 0)
		goto done;

	git_buf_rtrim(&current);

	if (state->merge.current)
		git_commit_free(state->merge.current);

	if ((error = git_oid_fromstr(&current_id, current.ptr)) < 0 ||
		(error = git_commit_lookup(&state->merge.current, repo, &current_id)) < 0 ||
		(error = git_commit_tree(&current_tree, state->merge.current)) < 0 ||
		(error = git_repository_head_tree(&head_tree, repo)) < 0)
		goto done;

	if ((parent_count = git_commit_parentcount(state->merge.current)) > 1) {
		giterr_set(GITERR_REBASE, "Cannot rebase a merge commit");
		error = -1;
		goto done;
	} else if (parent_count) {
		if ((error = git_commit_parent(&parent_commit, state->merge.current, 0)) < 0 ||
			(error = git_commit_tree(&parent_tree, parent_commit)) < 0)
			goto done;
	}

	if ((error = rebase_setupfile(repo, MSGNUM_FILE, -1, "%d\n", state->merge.msgnum)) < 0 ||
		(error = rebase_setupfile(repo, CURRENT_FILE, -1, "%s\n", current.ptr)) < 0)
		goto done;

	if ((error = normalize_checkout_opts(repo, &checkout_opts, given_checkout_opts, state)) < 0 ||
		(error = git_merge_trees(&index, repo, parent_tree, head_tree, current_tree, NULL)) < 0 ||
		(error = git_merge__check_result(repo, index)) < 0 ||
		(error = git_checkout_index(repo, index, &checkout_opts)) < 0)
		goto done;

done:
	git_index_free(index);
	git_tree_free(current_tree);
	git_tree_free(head_tree);
	git_tree_free(parent_tree);
	git_commit_free(parent_commit);
	git_buf_free(&path);
	git_buf_free(&current);

	return error;
}

int git_rebase_next(
	git_repository *repo,
	git_checkout_options *checkout_opts)
{
	git_rebase_state state = GIT_REBASE_STATE_INIT;
	int error;

	assert(repo);

	if ((error = rebase_state(&state, repo)) < 0)
		return -1;

	switch (state.type) {
	case GIT_REBASE_TYPE_MERGE:
		error = rebase_next_merge(repo, &state, checkout_opts);
		break;
	default:
		abort();
	}

	rebase_state_free(&state);
	return error;
}

static int rebase_commit_merge(
	git_oid *commit_id,
	git_repository *repo,
	git_rebase_state *state,
	const git_signature *author,
	const git_signature *committer,
	const char *message_encoding,
	const char *message)
{
	git_index *index = NULL;
	git_reference *head = NULL;
	git_commit *head_commit = NULL, *commit = NULL;
	git_tree *head_tree = NULL, *tree = NULL;
	git_diff *diff = NULL;
	git_oid tree_id;
	git_buf reflog_msg = GIT_BUF_INIT;
	char old_idstr[GIT_OID_HEXSZ], new_idstr[GIT_OID_HEXSZ];
	int error;

	if (!state->merge.msgnum || !state->merge.current) {
		giterr_set(GITERR_REBASE, "No rebase-merge state files exist");
		error = -1;
		goto done;
	}

	if ((error = git_repository_index(&index, repo)) < 0)
		goto done;

	if (git_index_has_conflicts(index)) {
		giterr_set(GITERR_REBASE, "Conflicts have not been resolved");
		error = GIT_EMERGECONFLICT;
		goto done;
	}

	if ((error = git_repository_head(&head, repo)) < 0 ||
		(error = git_reference_peel((git_object **)&head_commit, head, GIT_OBJ_COMMIT)) < 0 ||
		(error = git_commit_tree(&head_tree, head_commit)) < 0 ||
		(error = git_diff_tree_to_index(&diff, repo, head_tree, index, NULL)) < 0)
		goto done;

	if (git_diff_num_deltas(diff) == 0) {
		giterr_set(GITERR_REBASE, "This patch has already been applied");
		error = GIT_EAPPLIED;
		goto done;
	}

	if ((error = git_index_write_tree(&tree_id, index)) < 0 ||
		(error = git_tree_lookup(&tree, repo, &tree_id)) < 0)
		goto done;

	if (!author)
		author = git_commit_author(state->merge.current);

	if (!message) {
		message_encoding = git_commit_message_encoding(state->merge.current);
		message = git_commit_message(state->merge.current);
	}

	if ((error = git_commit_create(commit_id, repo, NULL, author,
			committer, message_encoding, message, tree, 1,
			(const git_commit **)&head_commit)) < 0 ||
		(error = git_commit_lookup(&commit, repo, commit_id)) < 0 ||
		(error = git_reference__update_for_commit(
			repo, NULL, "HEAD", commit_id, committer, "rebase")) < 0)
		goto done;

	git_oid_fmt(old_idstr, git_commit_id(state->merge.current));
	git_oid_fmt(new_idstr, commit_id);

	error = rebase_setupfile(repo, REWRITTEN_FILE, O_CREAT|O_WRONLY|O_APPEND,
		"%.*s %.*s\n", GIT_OID_HEXSZ, old_idstr, GIT_OID_HEXSZ, new_idstr);

done:
	git_buf_free(&reflog_msg);
	git_commit_free(commit);
	git_diff_free(diff);
	git_tree_free(tree);
	git_tree_free(head_tree);
	git_commit_free(head_commit);
	git_reference_free(head);
	git_index_free(index);

	return error;
}

int git_rebase_commit(
	git_oid *id,
	git_repository *repo,
	const git_signature *author,
	const git_signature *committer,
	const char *message_encoding,
	const char *message)
{
	git_rebase_state state = GIT_REBASE_STATE_INIT;
	int error;

	assert(repo && committer);

	if ((error = rebase_state(&state, repo)) < 0)
		goto done;

	switch (state.type) {
	case GIT_REBASE_TYPE_MERGE:
		error = rebase_commit_merge(
			id, repo, &state, author, committer, message_encoding, message);
		break;
	default:
		abort();
	}

done:
	rebase_state_free(&state);
	return error;
}

int git_rebase_abort(git_repository *repo, const git_signature *signature)
{
	git_rebase_state state = GIT_REBASE_STATE_INIT;
	git_reference *orig_head_ref = NULL;
	git_commit *orig_head_commit = NULL;
	int error;

	assert(repo && signature);

	if ((error = rebase_state(&state, repo)) < 0)
		goto done;

	error = state.head_detached ?
		git_reference_create(&orig_head_ref, repo, GIT_HEAD_FILE,
			 &state.orig_head_id, 1, signature, "rebase: aborting") :
		git_reference_symbolic_create(
			&orig_head_ref, repo, GIT_HEAD_FILE, state.orig_head_name, 1,
			signature, "rebase: aborting");

	if (error < 0)
		goto done;

	if ((error = git_commit_lookup(
			&orig_head_commit, repo, &state.orig_head_id)) < 0 ||
		(error = git_reset(repo, (git_object *)orig_head_commit,
			GIT_RESET_HARD, signature, NULL)) < 0)
		goto done;

	error = rebase_cleanup(&state);

done:
	git_commit_free(orig_head_commit);
	git_reference_free(orig_head_ref);
	rebase_state_free(&state);

	return error;
}

static int rebase_copy_note(
	git_repository *repo,
	git_oid *from,
	git_oid *to,
	const git_signature *committer,
	const git_rebase_options *opts)
{
	git_note *note = NULL;
	git_oid note_id;
	int error;

	if ((error = git_note_read(&note, repo, opts->rewrite_notes_ref, from)) < 0) {
		if (error == GIT_ENOTFOUND) {
			giterr_clear();
			error = 0;
		}

		goto done;
	}

	error = git_note_create(&note_id, repo, git_note_author(note),
		committer, opts->rewrite_notes_ref, to, git_note_message(note), 0);

done:
	git_note_free(note);

	return error;
}

static int rebase_copy_notes(
	git_repository *repo,
	git_rebase_state *state,
	const git_signature *committer,
	const git_rebase_options *opts)
{
	git_buf path = GIT_BUF_INIT, rewritten = GIT_BUF_INIT;
	char *pair_list, *fromstr, *tostr, *end;
	git_oid from, to;
	unsigned int linenum = 1;
	int error = 0;

	if (!opts->rewrite_notes_ref)
		goto done;

	if ((error = git_buf_joinpath(&path, state->state_path, REWRITTEN_FILE)) < 0 ||
		(error = git_futils_readbuffer(&rewritten, path.ptr)) < 0)
		goto done;

	pair_list = rewritten.ptr;

	while (*pair_list) {
		fromstr = pair_list;

		if ((end = strchr(fromstr, '\n')) == NULL)
			goto on_error;

		pair_list = end+1;
		*end = '\0';

		if ((end = strchr(fromstr, ' ')) == NULL)
			goto on_error;

		tostr = end+1;
		*end = '\0';

		if (strlen(fromstr) != GIT_OID_HEXSZ ||
			strlen(tostr) != GIT_OID_HEXSZ ||
			git_oid_fromstr(&from, fromstr) < 0 ||
			git_oid_fromstr(&to, tostr) < 0)
			goto on_error;

		if ((error = rebase_copy_note(repo, &from, &to, committer, opts)) < 0)
			goto done;

		linenum++;
	}

	goto done;

on_error:
	giterr_set(GITERR_REBASE, "Invalid rewritten file at line %d", linenum);
	error = -1;

done:
	git_buf_free(&rewritten);
	git_buf_free(&path);

	return error;
}

int git_rebase_finish(
	git_repository *repo,
	const git_signature *signature,
	const git_rebase_options *given_opts)
{
	git_rebase_options opts;
	git_rebase_state state = GIT_REBASE_STATE_INIT;
	git_reference *terminal_ref = NULL, *branch_ref = NULL, *head_ref = NULL;
	git_commit *terminal_commit = NULL;
	git_buf branch_msg = GIT_BUF_INIT, head_msg = GIT_BUF_INIT;
	char onto[GIT_OID_HEXSZ];
	int error;

	assert(repo);

	if ((error = rebase_normalize_opts(repo, &opts, given_opts)) < 0 ||
		(error = rebase_state(&state, repo)) < 0)
		goto done;

	git_oid_fmt(onto, &state.onto_id);

	if ((error = git_buf_printf(&branch_msg, "rebase finished: %s onto %.*s",
			state.orig_head_name, GIT_OID_HEXSZ, onto)) < 0 ||
		(error = git_buf_printf(&head_msg, "rebase finished: returning to %s",
			state.orig_head_name)) < 0 ||
		(error = git_repository_head(&terminal_ref, repo)) < 0 ||
		(error = git_reference_peel((git_object **)&terminal_commit,
			terminal_ref, GIT_OBJ_COMMIT)) < 0 ||
		(error = git_reference_create_matching(&branch_ref,
			repo, state.orig_head_name, git_commit_id(terminal_commit), 1,
			&state.orig_head_id, signature, branch_msg.ptr)) < 0 ||
		(error = git_reference_symbolic_create(&head_ref,
			repo, GIT_HEAD_FILE, state.orig_head_name, 1,
			signature, head_msg.ptr)) < 0 ||
		(error = rebase_copy_notes(repo, &state, signature, &opts)) < 0)
		goto done;

	error = rebase_cleanup(&state);

done:
	git_buf_free(&head_msg);
	git_buf_free(&branch_msg);
	git_commit_free(terminal_commit);
	git_reference_free(head_ref);
	git_reference_free(branch_ref);
	git_reference_free(terminal_ref);
	rebase_state_free(&state);
	rebase_opts_free(&opts);

	return error;
}

