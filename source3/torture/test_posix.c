/*
   Unix SMB/CIFS implementation.
   Copyright (C) Ralph Boehme 2020

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "torture/proto.h"
#include "libcli/security/security.h"
#include "libsmb/libsmb.h"
#include "libsmb/clirap.h"
#include "libsmb/proto.h"
#include "../libcli/smb/smbXcli_base.h"

extern struct cli_credentials *torture_creds;
extern fstring host, workgroup, share, password, username, myname;

struct posix_test_entry {
	const char *name;
	const char *target;
	const char *expected;
	uint32_t attr;
	uint64_t returned_size;
	bool ok;
};

static NTSTATUS posix_ls_fn(struct file_info *finfo,
			    const char *name,
			    void *_state)
{
	struct posix_test_entry *state =
		(struct posix_test_entry *)_state;

	for (; state->name != NULL; state++) {
		if (strequal(finfo->name, state->expected)) {
			if (state->attr != finfo->attr) {
				break;
			}
			state->ok = true;
			state->returned_size = finfo->size;
			break;
		}
	}

	return NT_STATUS_OK;
}

static void posix_test_entries_reset(struct posix_test_entry *state)
{
	for (; state->name != NULL; state++) {
		state->ok = false;
		state->returned_size = 0;
	}
}

static bool posix_test_entry_check(struct posix_test_entry *state,
				   const char *name,
				   bool expected,
				   uint64_t expected_size)
{
	bool result = false;

	for (; state->name != NULL; state++) {
		if (strequal(name, state->name)) {
			result = state->ok;
			break;
		}
	}
	if (state->name == NULL) {
		printf("test failed, unknown name: %s\n", name);
		return false;
	}

	if (expected == result) {
		return true;
	}

	printf("test failed, %s: %s\n",
	       expected ? "missing" : "unexpected",
	       name);

	return false;
}

/*
  Test non-POSIX vs POSIX ls * of symlinks
 */
bool run_posix_ls_wildcard_test(int dummy)
{
	TALLOC_CTX *frame = NULL;
	struct cli_state *cli_unix = NULL;
	struct cli_state *cli_win = NULL;
	uint16_t fnum = (uint16_t)-1;
	NTSTATUS status;
	const char *file = "file";
	const char *symlnk_dangling = "dangling";
	const char *symlnk_dst_dangling = "xxxxxxx";
	const char *symlnk_in_share = "symlnk_in_share";
	const char *symlnk_dst_in_share = file;
	const char *symlnk_outside_share = "symlnk_outside_share";
	const char *symlnk_dst_outside_share = "/etc/passwd";
	struct posix_test_entry state[] = {
		{
			.name = symlnk_dangling,
			.target = symlnk_dst_dangling,
			.expected = symlnk_dangling,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = symlnk_in_share,
			.target = symlnk_dst_in_share,
			.expected = symlnk_in_share,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = symlnk_outside_share,
			.target = symlnk_dst_outside_share,
			.expected = symlnk_outside_share,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = NULL,
		}
	};
	int i;
	bool correct = false;

	frame = talloc_stackframe();

	printf("Starting POSIX-LS-WILDCARD test\n");

	if (!torture_open_connection(&cli_unix, 0)) {
		TALLOC_FREE(frame);
		return false;
	}

	if (!torture_open_connection(&cli_win, 0)) {
		TALLOC_FREE(frame);
		return false;
	}

	torture_conn_set_sockopt(cli_unix);
	torture_conn_set_sockopt(cli_win);

	status = torture_setup_unix_extensions(cli_unix);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(frame);
		return false;
	}

	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	status = cli_posix_open(cli_unix,
				file,
				O_RDWR|O_CREAT,
				0666,
				&fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_open of %s failed error %s\n",
		       file,
		       nt_errstr(status));
		goto out;
	}

	status = cli_close(cli_unix, fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}
	fnum = (uint16_t)-1;

	for (i = 0; state[i].name != NULL; i++) {
		status = cli_posix_symlink(cli_unix,
					   state[i].target,
					   state[i].name);
		if (!NT_STATUS_IS_OK(status)) {
			printf("POSIX symlink of %s failed (%s)\n",
			       symlnk_dangling, nt_errstr(status));
			goto out;
		}
	}

	printf("Doing Windows ls *\n");

	status = cli_list(cli_win, "*", 0, posix_ls_fn, state);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}

	if (!posix_test_entry_check(state, symlnk_dangling, false, 0)) {
		goto out;
	}
	if (!posix_test_entry_check(state, symlnk_outside_share, false, 0)) {
		goto out;
	}
	if (!posix_test_entry_check(state, symlnk_in_share, true, 0)) {
		goto out;
	}

	posix_test_entries_reset(state);

	printf("Doing POSIX ls *\n");

	status = cli_list(cli_unix, "*", 0, posix_ls_fn, state);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}

	if (!posix_test_entry_check(state,
				    symlnk_dangling,
				    true,
				    strlen(symlnk_dst_dangling)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_outside_share,
				    true,
				    strlen(symlnk_dst_outside_share)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_in_share,
				    true,
				    strlen(symlnk_dst_in_share))) {
		goto out;
	}

	printf("POSIX-LS-WILDCARD test passed\n");
	correct = true;

out:
	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	if (!torture_close_connection(cli_unix)) {
		correct = false;
	}
	if (!torture_close_connection(cli_win)) {
		correct = false;
	}

	TALLOC_FREE(frame);
	return correct;
}

/*
  Test non-POSIX vs POSIX ls single of symlinks
 */
bool run_posix_ls_single_test(int dummy)
{
	TALLOC_CTX *frame = NULL;
	struct cli_state *cli_unix = NULL;
	struct cli_state *cli_win = NULL;
	uint16_t fnum = (uint16_t)-1;
	NTSTATUS status;
	const char *file = "file";
	const char *symlnk_dangling = "dangling";
	const char *symlnk_dst_dangling = "xxxxxxx";
	const char *symlnk_in_share = "symlnk_in_share";
	const char *symlnk_dst_in_share = file;
	const char *symlnk_outside_share = "symlnk_outside_share";
	const char *symlnk_dst_outside_share = "/etc/passwd";
	struct posix_test_entry state[] = {
		{
			.name = symlnk_dangling,
			.target = symlnk_dst_dangling,
			.expected = symlnk_dangling,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = symlnk_in_share,
			.target = symlnk_dst_in_share,
			.expected = symlnk_in_share,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = symlnk_outside_share,
			.target = symlnk_dst_outside_share,
			.expected = symlnk_outside_share,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = NULL,
		}
	};
	int i;
	bool correct = false;

	frame = talloc_stackframe();

	printf("Starting POSIX-LS-SINGLE test\n");

	if (!torture_open_connection(&cli_unix, 0)) {
		TALLOC_FREE(frame);
		return false;
	}

	if (!torture_init_connection(&cli_win)) {
		TALLOC_FREE(frame);
		return false;
	}

	status = smbXcli_negprot(cli_win->conn,
				 cli_win->timeout,
				 lp_client_min_protocol(),
				 lp_client_max_protocol());
	if (!NT_STATUS_IS_OK(status)) {
		printf("smbXcli_negprot returned %s\n", nt_errstr(status));
		TALLOC_FREE(frame);
		return false;
	}

	status = cli_session_setup_creds(cli_win, torture_creds);
	if (!NT_STATUS_IS_OK(status)) {
		printf("smb2cli_sesssetup returned %s\n", nt_errstr(status));
		TALLOC_FREE(frame);
		return false;
	}

	status = cli_tree_connect(cli_win, share, "?????", NULL);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_tree_connect returned %s\n", nt_errstr(status));
		TALLOC_FREE(frame);
		return false;
	}
	torture_conn_set_sockopt(cli_unix);
	torture_conn_set_sockopt(cli_win);

	status = torture_setup_unix_extensions(cli_unix);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(frame);
		return false;
	}

	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	status = cli_posix_open(cli_unix,
				file,
				O_RDWR|O_CREAT,
				0666,
				&fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_open of %s failed error %s\n",
		       file,
		       nt_errstr(status));
		goto out;
	}

	status = cli_close(cli_unix, fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}
	fnum = (uint16_t)-1;

	for (i = 0; state[i].name != NULL; i++) {
		status = cli_posix_symlink(cli_unix,
					   state[i].target,
					   state[i].name);
		if (!NT_STATUS_IS_OK(status)) {
			printf("POSIX symlink of %s failed (%s)\n",
			       symlnk_dangling, nt_errstr(status));
			goto out;
		}
	}

	printf("Doing Windows ls single\n");

	cli_list(cli_win, symlnk_dangling, 0, posix_ls_fn, state);
	cli_list(cli_win, symlnk_outside_share, 0, posix_ls_fn, state);
	cli_list(cli_win, symlnk_in_share, 0, posix_ls_fn, state);

	if (!posix_test_entry_check(state, symlnk_dangling, false, 0)) {
		goto out;
	}
	if (!posix_test_entry_check(state, symlnk_outside_share, false, 0)) {
		goto out;
	}
	if (!posix_test_entry_check(state, symlnk_in_share, true, 0)) {
		goto out;
	}

	posix_test_entries_reset(state);

	printf("Doing POSIX ls single\n");

	cli_list(cli_unix, symlnk_dangling, 0, posix_ls_fn, state);
	cli_list(cli_unix, symlnk_outside_share, 0, posix_ls_fn, state);
	cli_list(cli_unix, symlnk_in_share, 0, posix_ls_fn, state);

	if (!posix_test_entry_check(state,
				    symlnk_dangling,
				    true,
				    strlen(symlnk_dst_dangling)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_outside_share,
				    true,
				    strlen(symlnk_dst_outside_share)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_in_share,
				    true,
				    strlen(symlnk_dst_in_share))) {
		goto out;
	}

	printf("POSIX-LS-SINGLE test passed\n");
	correct = true;

out:
	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	if (!torture_close_connection(cli_unix)) {
		correct = false;
	}
	if (!torture_close_connection(cli_win)) {
		correct = false;
	}

	TALLOC_FREE(frame);
	return correct;
}

/*
  Test POSIX readlink of symlinks
 */
bool run_posix_readlink_test(int dummy)
{
	TALLOC_CTX *frame = NULL;
	struct cli_state *cli_unix = NULL;
	uint16_t fnum = (uint16_t)-1;
	NTSTATUS status;
	const char *file = "file";
	const char *symlnk_dangling = "dangling";
	const char *symlnk_dst_dangling = "xxxxxxx";
	const char *symlnk_in_share = "symlnk_in_share";
	const char *symlnk_dst_in_share = file;
	const char *symlnk_outside_share = "symlnk_outside_share";
	const char *symlnk_dst_outside_share = "/etc/passwd";
	struct posix_test_entry state[] = {
		{
			.name = symlnk_dangling,
			.target = symlnk_dst_dangling,
			.expected = symlnk_dangling,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = symlnk_in_share,
			.target = symlnk_dst_in_share,
			.expected = symlnk_in_share,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = symlnk_outside_share,
			.target = symlnk_dst_outside_share,
			.expected = symlnk_outside_share,
			.attr = FILE_ATTRIBUTE_NORMAL,
		}, {
			.name = NULL,
		}
	};
	int i;
	bool correct = false;

	frame = talloc_stackframe();

	printf("Starting POSIX-READLINK test\n");

	if (!torture_open_connection(&cli_unix, 0)) {
		TALLOC_FREE(frame);
		return false;
	}

	torture_conn_set_sockopt(cli_unix);

	status = torture_setup_unix_extensions(cli_unix);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(frame);
		return false;
	}

	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	status = cli_posix_open(cli_unix,
				file,
				O_RDWR|O_CREAT,
				0666,
				&fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_open of %s failed error %s\n",
		       file,
		       nt_errstr(status));
		goto out;
	}

	status = cli_close(cli_unix, fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}
	fnum = (uint16_t)-1;

	for (i = 0; state[i].name != NULL; i++) {
		status = cli_posix_symlink(cli_unix,
					   state[i].target,
					   state[i].name);
		if (!NT_STATUS_IS_OK(status)) {
			printf("POSIX symlink of %s failed (%s)\n",
			       symlnk_dangling, nt_errstr(status));
			goto out;
		}
	}

	for (i = 0; state[i].name != NULL; i++) {
		char *target = NULL;

		status = cli_posix_readlink(cli_unix,
					    state[i].name,
					    talloc_tos(),
					    &target);
		if (!NT_STATUS_IS_OK(status)) {
			printf("POSIX readlink on %s failed (%s)\n",
			       state[i].name, nt_errstr(status));
			goto out;
		}
		if (strequal(target, state[i].target)) {
			state[i].ok = true;
			state[i].returned_size = strlen(target);
		}
	}

	if (!posix_test_entry_check(state,
				    symlnk_dangling,
				    true,
				    strlen(symlnk_dst_dangling)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_outside_share,
				    true,
				    strlen(symlnk_dst_outside_share)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_in_share,
				    true,
				    strlen(symlnk_dst_in_share))) {
		goto out;
	}

	printf("POSIX-READLINK test passed\n");
	correct = true;

out:
	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	if (!torture_close_connection(cli_unix)) {
		correct = false;
	}

	TALLOC_FREE(frame);
	return correct;
}

/*
  Test POSIX stat of symlinks
 */
bool run_posix_stat_test(int dummy)
{
	TALLOC_CTX *frame = NULL;
	struct cli_state *cli_unix = NULL;
	uint16_t fnum = (uint16_t)-1;
	NTSTATUS status;
	const char *file = "file";
	const char *symlnk_dangling = "dangling";
	const char *symlnk_dst_dangling = "xxxxxxx";
	const char *symlnk_in_share = "symlnk_in_share";
	const char *symlnk_dst_in_share = file;
	const char *symlnk_outside_share = "symlnk_outside_share";
	const char *symlnk_dst_outside_share = "/etc/passwd";
	struct posix_test_entry state[] = {
		{
			.name = symlnk_dangling,
			.target = symlnk_dst_dangling,
			.expected = symlnk_dangling,
		}, {
			.name = symlnk_in_share,
			.target = symlnk_dst_in_share,
			.expected = symlnk_in_share,
		}, {
			.name = symlnk_outside_share,
			.target = symlnk_dst_outside_share,
			.expected = symlnk_outside_share,
		}, {
			.name = NULL,
		}
	};
	int i;
	bool correct = false;

	frame = talloc_stackframe();

	printf("Starting POSIX-STAT test\n");

	if (!torture_open_connection(&cli_unix, 0)) {
		TALLOC_FREE(frame);
		return false;
	}

	torture_conn_set_sockopt(cli_unix);

	status = torture_setup_unix_extensions(cli_unix);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(frame);
		return false;
	}

	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	status = cli_posix_open(cli_unix,
				file,
				O_RDWR|O_CREAT,
				0666,
				&fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_open of %s failed error %s\n",
		       file,
		       nt_errstr(status));
		goto out;
	}

	status = cli_close(cli_unix, fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}
	fnum = (uint16_t)-1;

	for (i = 0; state[i].name != NULL; i++) {
		status = cli_posix_symlink(cli_unix,
					   state[i].target,
					   state[i].name);
		if (!NT_STATUS_IS_OK(status)) {
			printf("POSIX symlink of %s failed (%s)\n",
			       symlnk_dangling, nt_errstr(status));
			goto out;
		}
	}

	for (i = 0; state[i].name != NULL; i++) {
		SMB_STRUCT_STAT sbuf;

		status = cli_posix_stat(cli_unix,
					state[i].name,
					&sbuf);
		if (!NT_STATUS_IS_OK(status)) {
			printf("POSIX stat on %s failed (%s)\n",
			       state[i].name, nt_errstr(status));
			continue;
		}
		state[i].ok = true;
		state[i].returned_size = sbuf.st_ex_size;
	}

	if (!posix_test_entry_check(state,
				    symlnk_dangling,
				    true,
				    strlen(symlnk_dst_dangling)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_outside_share,
				    true,
				    strlen(symlnk_dst_outside_share)))
	{
		goto out;
	}
	if (!posix_test_entry_check(state,
				    symlnk_in_share,
				    true,
				    strlen(symlnk_dst_in_share))) {
		goto out;
	}

	printf("POSIX-STAT test passed\n");
	correct = true;

out:
	cli_posix_unlink(cli_unix, file);
	cli_posix_unlink(cli_unix, symlnk_dangling);
	cli_posix_unlink(cli_unix, symlnk_in_share);
	cli_posix_unlink(cli_unix, symlnk_outside_share);

	if (!torture_close_connection(cli_unix)) {
		correct = false;
	}

	TALLOC_FREE(frame);
	return correct;
}

/*
  Test Creating files and directories directly
  under a symlink.
 */
bool run_posix_symlink_parent_test(int dummy)
{
	TALLOC_CTX *frame = NULL;
	struct cli_state *cli_unix = NULL;
	uint16_t fnum = (uint16_t)-1;
	NTSTATUS status;
	const char *parent_dir = "target_dir";
	const char *parent_symlink = "symlink_to_target_dir";
	const char *fname_real = "target_dir/file";
	const char *dname_real = "target_dir/dir";
	const char *fname_link = "symlink_to_target_dir/file";
	const char *dname_link = "symlink_to_target_dir/dir";
	const char *sname_link = "symlink_to_target_dir/symlink";
	const char *hname_link = "symlink_to_target_dir/hardlink";
	bool correct = false;

	frame = talloc_stackframe();

	printf("Starting POSIX-SYMLINK-PARENT test\n");

	if (!torture_open_connection(&cli_unix, 0)) {
		TALLOC_FREE(frame);
		return false;
	}

	torture_conn_set_sockopt(cli_unix);

	status = torture_setup_unix_extensions(cli_unix);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(frame);
		return false;
	}

	/* Start with a clean slate. */
	cli_posix_unlink(cli_unix, fname_real);
	cli_posix_rmdir(cli_unix, dname_real);
	cli_posix_unlink(cli_unix, fname_link);
	cli_posix_rmdir(cli_unix, dname_link);
	cli_posix_unlink(cli_unix, sname_link);
	cli_posix_unlink(cli_unix, hname_link);
	cli_posix_unlink(cli_unix, parent_symlink);
	cli_posix_rmdir(cli_unix, parent_dir);

	/* Create parent_dir. */
	status = cli_posix_mkdir(cli_unix, parent_dir, 0777);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_mkdir of %s failed error %s\n",
		       parent_dir,
		       nt_errstr(status));
		goto out;
	}
	/* Create symlink to parent_dir. */
	status = cli_posix_symlink(cli_unix,
				   parent_dir,
				   parent_symlink);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_symlink of %s -> %s failed error %s\n",
		       parent_symlink,
		       parent_dir,
		       nt_errstr(status));
		goto out;
	}
	/* Try and create a directory under the symlink. */
	status = cli_posix_mkdir(cli_unix, dname_link, 0777);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_mkdir of %s failed error %s\n",
		       dname_link,
		       nt_errstr(status));
		goto out;
	}
	/* Try and create a file under the symlink. */
	status = cli_posix_open(cli_unix,
				fname_link,
				O_RDWR|O_CREAT,
				0666,
				&fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_open of %s failed error %s\n",
		       fname_link,
		       nt_errstr(status));
		goto out;
	}
	status = cli_close(cli_unix, fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}
	fnum = (uint16_t)-1;

	/* Try and create a symlink to the file under the symlink. */
	status = cli_posix_symlink(cli_unix,
				   fname_link,
				   sname_link);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_symlink of %s -> %s failed error %s\n",
			sname_link,
			fname_link,
			nt_errstr(status));
		goto out;
	}

	/* Try and create a hardlink to the file under the symlink. */
	status = cli_posix_hardlink(cli_unix,
				   fname_link,
				   hname_link);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_hardlink of %s -> %s failed error %s\n",
			hname_link,
			fname_link,
			nt_errstr(status));
		goto out;
	}

	/* Ensure we can delete the symlink via the parent symlink */
	status = cli_posix_unlink(cli_unix, sname_link);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_unlink of %s failed error %s\n",
		       sname_link,
		       nt_errstr(status));
		goto out;
	}

	/* Ensure we can delete the hardlink via the parent symlink */
	status = cli_posix_unlink(cli_unix, hname_link);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_unlink of %s failed error %s\n",
		       hname_link,
		       nt_errstr(status));
		goto out;
	}

	/* Ensure we can delete the directory via the parent symlink */
	status = cli_posix_rmdir(cli_unix, dname_link);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_rmdir of %s failed error %s\n",
		       dname_link,
		       nt_errstr(status));
		goto out;
	}
	/* Ensure we can delete the file via the parent symlink */
	status = cli_posix_unlink(cli_unix, fname_link);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_unlink of %s failed error %s\n",
		       fname_link,
		       nt_errstr(status));
		goto out;
	}

	printf("POSIX-SYMLINK-PARENT test passed\n");
	correct = true;

out:
	if (fnum != (uint16_t)-1) {
		cli_close(cli_unix, fnum);
	}
	cli_posix_unlink(cli_unix, fname_real);
	cli_posix_rmdir(cli_unix, dname_real);
	cli_posix_unlink(cli_unix, fname_link);
	cli_posix_rmdir(cli_unix, dname_link);
	cli_posix_unlink(cli_unix, sname_link);
	cli_posix_unlink(cli_unix, hname_link);
	cli_posix_unlink(cli_unix, parent_symlink);
	cli_posix_rmdir(cli_unix, parent_dir);

	if (!torture_close_connection(cli_unix)) {
		correct = false;
	}

	TALLOC_FREE(frame);
	return correct;
}

/*
  Ensure we get an error when doing chmod on a symlink,
  whether it is pointing to a real object or dangling.
 */
bool run_posix_symlink_chmod_test(int dummy)
{
	TALLOC_CTX *frame = NULL;
	struct cli_state *cli_unix = NULL;
	NTSTATUS status;
	uint16_t fnum = (uint16_t)-1;
	const char *fname_real = "file_real";
	const char *fname_real_symlink = "file_real_symlink";
	const char *nonexist = "nonexist";
	const char *nonexist_symlink = "dangling_symlink";
	bool correct = false;

	frame = talloc_stackframe();

	printf("Starting POSIX-SYMLINK-CHMOD test\n");

	if (!torture_open_connection(&cli_unix, 0)) {
		TALLOC_FREE(frame);
		return false;
	}

	torture_conn_set_sockopt(cli_unix);

	status = torture_setup_unix_extensions(cli_unix);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(frame);
		return false;
	}

	/* Start with a clean slate. */
	cli_posix_unlink(cli_unix, fname_real);
	cli_posix_unlink(cli_unix, fname_real_symlink);
	cli_posix_unlink(cli_unix, nonexist);
	cli_posix_unlink(cli_unix, nonexist_symlink);

	/* Create a real file. */
	status = cli_posix_open(cli_unix,
				fname_real,
				O_RDWR|O_CREAT,
				0644,
				&fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_open of %s failed error %s\n",
		       fname_real,
		       nt_errstr(status));
		goto out;
	}
	status = cli_close(cli_unix, fnum);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_close failed %s\n", nt_errstr(status));
		goto out;
	}
	fnum = (uint16_t)-1;

	/* Create symlink to real target. */
	status = cli_posix_symlink(cli_unix,
				   fname_real,
				   fname_real_symlink);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_symlink of %s -> %s failed error %s\n",
		       fname_real_symlink,
		       fname_real,
		       nt_errstr(status));
		goto out;
	}

	/* We should not be able to chmod symlinks that point to something. */
	status = cli_posix_chmod(cli_unix, fname_real_symlink, 0777);

	/* This should fail with something other than server crashed. */
	if (NT_STATUS_IS_OK(status)) {
		printf("cli_posix_chmod of %s succeeded (should have failed)\n",
			fname_real_symlink);
		goto out;
	}
	if (NT_STATUS_EQUAL(status, NT_STATUS_CONNECTION_DISCONNECTED)) {
		/* Oops. Server crashed. */
		printf("cli_posix_chmod of %s failed error %s\n",
			fname_real_symlink,
			nt_errstr(status));
		goto out;
	}
	/* Any other failure is ok. */

	/* Now create symlink to non-existing target. */
	status = cli_posix_symlink(cli_unix,
				   nonexist,
				   nonexist_symlink);
	if (!NT_STATUS_IS_OK(status)) {
		printf("cli_posix_symlink of %s -> %s failed error %s\n",
		       nonexist_symlink,
		       nonexist,
		       nt_errstr(status));
		goto out;
	}

	/* We should not be able to chmod symlinks that point to nothing. */
	status = cli_posix_chmod(cli_unix, nonexist_symlink, 0777);

	/* This should fail with something other than server crashed. */
	if (NT_STATUS_IS_OK(status)) {
		printf("cli_posix_chmod of %s succeeded (should have failed)\n",
			nonexist_symlink);
		goto out;
	}
	if (NT_STATUS_EQUAL(status, NT_STATUS_CONNECTION_DISCONNECTED)) {
		/* Oops. Server crashed. */
		printf("cli_posix_chmod of %s failed error %s\n",
			nonexist_symlink,
			nt_errstr(status));
		goto out;
	}

	/* Any other failure is ok. */
	printf("POSIX-SYMLINK-CHMOD test passed (expected failure was %s)\n",
			nt_errstr(status));
	correct = true;

out:
	if (fnum != (uint16_t)-1) {
		cli_close(cli_unix, fnum);
	}
	cli_posix_unlink(cli_unix, fname_real);
	cli_posix_unlink(cli_unix, fname_real_symlink);
	cli_posix_unlink(cli_unix, nonexist);
	cli_posix_unlink(cli_unix, nonexist_symlink);

	if (!torture_close_connection(cli_unix)) {
		correct = false;
	}

	TALLOC_FREE(frame);
	return correct;
}
