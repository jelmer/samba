/*
   Unix SMB/CIFS implementation.
   SMB NT transaction handling
   Copyright (C) Jeremy Allison			1994-2007
   Copyright (C) Stefan (metze) Metzmacher	2003

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

extern int max_send;
extern enum protocol_types Protocol;
extern int smb_read_error;
extern struct current_user current_user;

static const char *known_nt_pipes[] = {
	"\\LANMAN",
	"\\srvsvc",
	"\\samr",
	"\\wkssvc",
	"\\NETLOGON",
	"\\ntlsa",
	"\\ntsvcs",
	"\\lsass",
	"\\lsarpc",
	"\\winreg",
	"\\initshutdown",
	"\\spoolss",
	"\\netdfs",
	"\\rpcecho",
        "\\svcctl",
	"\\eventlog",
	"\\unixinfo",
	NULL
};

static char *nttrans_realloc(char **ptr, size_t size)
{
	if (ptr==NULL) {
		smb_panic("nttrans_realloc() called with NULL ptr");
	}

	*ptr = (char *)SMB_REALLOC(*ptr, size);
	if(*ptr == NULL) {
		return NULL;
	}
	memset(*ptr,'\0',size);
	return *ptr;
}

/****************************************************************************
 Send the required number of replies back.
 We assume all fields other than the data fields are
 set correctly for the type of call.
 HACK ! Always assumes smb_setup field is zero.
****************************************************************************/

void send_nt_replies(struct smb_request *req, NTSTATUS nt_error,
		     char *params, int paramsize,
		     char *pdata, int datasize)
{
	int data_to_send = datasize;
	int params_to_send = paramsize;
	int useable_space;
	char *pp = params;
	char *pd = pdata;
	int params_sent_thistime, data_sent_thistime, total_sent_thistime;
	int alignment_offset = 3;
	int data_alignment_offset = 0;

	/*
	 * If there genuinely are no parameters or data to send just send
	 * the empty packet.
	 */

	if(params_to_send == 0 && data_to_send == 0) {
		reply_outbuf(req, 18, 0);
		show_msg((char *)req->outbuf);
		return;
	}

	/*
	 * When sending params and data ensure that both are nicely aligned.
	 * Only do this alignment when there is also data to send - else
	 * can cause NT redirector problems.
	 */

	if (((params_to_send % 4) != 0) && (data_to_send != 0)) {
		data_alignment_offset = 4 - (params_to_send % 4);
	}

	/*
	 * Space is bufsize minus Netbios over TCP header minus SMB header.
	 * The alignment_offset is to align the param bytes on a four byte
	 * boundary (2 bytes for data len, one byte pad).
	 * NT needs this to work correctly.
	 */

	useable_space = max_send - (smb_size
				    + 2 * 18 /* wct */
				    + alignment_offset
				    + data_alignment_offset);

	/*
	 * useable_space can never be more than max_send minus the
	 * alignment offset.
	 */

	useable_space = MIN(useable_space,
				max_send - (alignment_offset+data_alignment_offset));


	while (params_to_send || data_to_send) {

		/*
		 * Calculate whether we will totally or partially fill this packet.
		 */

		total_sent_thistime = params_to_send + data_to_send +
					alignment_offset + data_alignment_offset;

		/*
		 * We can never send more than useable_space.
		 */

		total_sent_thistime = MIN(total_sent_thistime, useable_space);

		reply_outbuf(req, 18, total_sent_thistime);

		/*
		 * Set total params and data to be sent.
		 */

		SIVAL(req->outbuf,smb_ntr_TotalParameterCount,paramsize);
		SIVAL(req->outbuf,smb_ntr_TotalDataCount,datasize);

		/*
		 * Calculate how many parameters and data we can fit into
		 * this packet. Parameters get precedence.
		 */

		params_sent_thistime = MIN(params_to_send,useable_space);
		data_sent_thistime = useable_space - params_sent_thistime;
		data_sent_thistime = MIN(data_sent_thistime,data_to_send);

		SIVAL(req->outbuf, smb_ntr_ParameterCount,
		      params_sent_thistime);

		if(params_sent_thistime == 0) {
			SIVAL(req->outbuf,smb_ntr_ParameterOffset,0);
			SIVAL(req->outbuf,smb_ntr_ParameterDisplacement,0);
		} else {
			/*
			 * smb_ntr_ParameterOffset is the offset from the start of the SMB header to the
			 * parameter bytes, however the first 4 bytes of outbuf are
			 * the Netbios over TCP header. Thus use smb_base() to subtract
			 * them from the calculation.
			 */

			SIVAL(req->outbuf,smb_ntr_ParameterOffset,
			      ((smb_buf(req->outbuf)+alignment_offset)
			       - smb_base(req->outbuf)));
			/*
			 * Absolute displacement of param bytes sent in this packet.
			 */

			SIVAL(req->outbuf, smb_ntr_ParameterDisplacement,
			      pp - params);
		}

		/*
		 * Deal with the data portion.
		 */

		SIVAL(req->outbuf, smb_ntr_DataCount, data_sent_thistime);

		if(data_sent_thistime == 0) {
			SIVAL(req->outbuf,smb_ntr_DataOffset,0);
			SIVAL(req->outbuf,smb_ntr_DataDisplacement, 0);
		} else {
			/*
			 * The offset of the data bytes is the offset of the
			 * parameter bytes plus the number of parameters being sent this time.
			 */

			SIVAL(req->outbuf, smb_ntr_DataOffset,
			      ((smb_buf(req->outbuf)+alignment_offset) -
			       smb_base(req->outbuf))
			      + params_sent_thistime + data_alignment_offset);
			SIVAL(req->outbuf,smb_ntr_DataDisplacement, pd - pdata);
		}

		/*
		 * Copy the param bytes into the packet.
		 */

		if(params_sent_thistime) {
			if (alignment_offset != 0) {
				memset(smb_buf(req->outbuf), 0,
				       alignment_offset);
			}
			memcpy((smb_buf(req->outbuf)+alignment_offset), pp,
			       params_sent_thistime);
		}

		/*
		 * Copy in the data bytes
		 */

		if(data_sent_thistime) {
			if (data_alignment_offset != 0) {
				memset((smb_buf(req->outbuf)+alignment_offset+
					params_sent_thistime), 0,
				       data_alignment_offset);
			}
			memcpy(smb_buf(req->outbuf)+alignment_offset
			       +params_sent_thistime+data_alignment_offset,
			       pd,data_sent_thistime);
		}

		DEBUG(9,("nt_rep: params_sent_thistime = %d, data_sent_thistime = %d, useable_space = %d\n",
			params_sent_thistime, data_sent_thistime, useable_space));
		DEBUG(9,("nt_rep: params_to_send = %d, data_to_send = %d, paramsize = %d, datasize = %d\n",
			params_to_send, data_to_send, paramsize, datasize));

		if (NT_STATUS_V(nt_error)) {
			error_packet_set((char *)req->outbuf,
					 0, 0, nt_error,
					 __LINE__,__FILE__);
		}

		/* Send the packet */
		show_msg((char *)req->outbuf);
		if (!send_smb(smbd_server_fd(),(char *)req->outbuf)) {
			exit_server_cleanly("send_nt_replies: send_smb failed.");
		}

		TALLOC_FREE(req->outbuf);

		pp += params_sent_thistime;
		pd += data_sent_thistime;

		params_to_send -= params_sent_thistime;
		data_to_send -= data_sent_thistime;

		/*
		 * Sanity check
		 */

		if(params_to_send < 0 || data_to_send < 0) {
			DEBUG(0,("send_nt_replies failed sanity check pts = %d, dts = %d\n!!!",
				params_to_send, data_to_send));
			return;
		}
	}
}

/****************************************************************************
 Is it an NTFS stream name ?
****************************************************************************/

bool is_ntfs_stream_name(const char *fname)
{
	if (lp_posix_pathnames()) {
		return False;
	}
	return (strchr_m(fname, ':') != NULL) ? True : False;
}

struct case_semantics_state {
	connection_struct *conn;
	bool case_sensitive;
	bool case_preserve;
	bool short_case_preserve;
};

/****************************************************************************
 Restore case semantics.
****************************************************************************/
static int restore_case_semantics(struct case_semantics_state *state)
{
	state->conn->case_sensitive = state->case_sensitive;
	state->conn->case_preserve = state->case_preserve;
	state->conn->short_case_preserve = state->short_case_preserve;
	return 0;
}

/****************************************************************************
 Save case semantics.
****************************************************************************/
static struct case_semantics_state *set_posix_case_semantics(TALLOC_CTX *mem_ctx,
							     connection_struct *conn)
{
	struct case_semantics_state *result;

	if (!(result = talloc(mem_ctx, struct case_semantics_state))) {
		DEBUG(0, ("talloc failed\n"));
		return NULL;
	}

	result->conn = conn;
	result->case_sensitive = conn->case_sensitive;
	result->case_preserve = conn->case_preserve;
	result->short_case_preserve = conn->short_case_preserve;

	/* Set to POSIX. */
	conn->case_sensitive = True;
	conn->case_preserve = True;
	conn->short_case_preserve = True;

	talloc_set_destructor(result, restore_case_semantics);

	return result;
}

/****************************************************************************
 Reply to an NT create and X call on a pipe
****************************************************************************/

static void nt_open_pipe(char *fname, connection_struct *conn,
			 struct smb_request *req, int *ppnum)
{
	smb_np_struct *p = NULL;
	int i;

	DEBUG(4,("nt_open_pipe: Opening pipe %s.\n", fname));

	/* See if it is one we want to handle. */

	if (lp_disable_spoolss() && strequal(fname, "\\spoolss")) {
		reply_botherror(req, NT_STATUS_OBJECT_NAME_NOT_FOUND,
				ERRDOS, ERRbadpipe);
		return;
	}

	for( i = 0; known_nt_pipes[i]; i++ ) {
		if( strequal(fname,known_nt_pipes[i])) {
			break;
		}
	}

	if ( known_nt_pipes[i] == NULL ) {
		reply_botherror(req, NT_STATUS_OBJECT_NAME_NOT_FOUND,
				ERRDOS, ERRbadpipe);
		return;
	}

	/* Strip \\ off the name. */
	fname++;

	DEBUG(3,("nt_open_pipe: Known pipe %s opening.\n", fname));

	p = open_rpc_pipe_p(fname, conn, req->vuid);
	if (!p) {
		reply_doserror(req, ERRSRV, ERRnofids);
		return;
	}

	/* TODO: Add pipe to db */

	if ( !store_pipe_opendb( p ) ) {
		DEBUG(3,("nt_open_pipe: failed to store %s pipe open.\n", fname));
	}

	*ppnum = p->pnum;
	return;
}

/****************************************************************************
 Reply to an NT create and X call for pipes.
****************************************************************************/

static void do_ntcreate_pipe_open(connection_struct *conn,
				  struct smb_request *req)
{
	char *fname = NULL;
	int pnum = -1;
	char *p = NULL;
	uint32 flags = IVAL(req->inbuf,smb_ntcreate_Flags);
	TALLOC_CTX *ctx = talloc_tos();

	srvstr_pull_buf_talloc(ctx, (char *)req->inbuf, req->flags2, &fname,
			smb_buf(req->inbuf), STR_TERMINATE);

	if (!fname) {
		reply_botherror(req, NT_STATUS_OBJECT_NAME_NOT_FOUND,
				ERRDOS, ERRbadpipe);
		return;
	}
	nt_open_pipe(fname, conn, req, &pnum);

	if (req->outbuf) {
		/* error reply */
		return;
	}

	/*
	 * Deal with pipe return.
	 */

	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		/* This is very strange. We
 		 * return 50 words, but only set
 		 * the wcnt to 42 ? It's definately
 		 * what happens on the wire....
 		 */
		reply_outbuf(req, 50, 0);
		SCVAL(req->outbuf,smb_wct,42);
	} else {
		reply_outbuf(req, 34, 0);
	}

	p = (char *)req->outbuf + smb_vwv2;
	p++;
	SSVAL(p,0,pnum);
	p += 2;
	SIVAL(p,0,FILE_WAS_OPENED);
	p += 4;
	p += 32;
	SIVAL(p,0,FILE_ATTRIBUTE_NORMAL); /* File Attributes. */
	p += 20;
	/* File type. */
	SSVAL(p,0,FILE_TYPE_MESSAGE_MODE_PIPE);
	/* Device state. */
	SSVAL(p,2, 0x5FF); /* ? */
	p += 4;

	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		p += 25;
		SIVAL(p,0,FILE_GENERIC_ALL);
		/*
		 * For pipes W2K3 seems to return
 		 * 0x12019B next.
 		 * This is ((FILE_GENERIC_READ|FILE_GENERIC_WRITE) & ~FILE_APPEND_DATA)
 		 */
		SIVAL(p,4,(FILE_GENERIC_READ|FILE_GENERIC_WRITE)&~FILE_APPEND_DATA);
	}

	DEBUG(5,("do_ntcreate_pipe_open: open pipe = %s\n", fname));

	chain_reply(req);
}

/****************************************************************************
 Reply to an NT create and X call for a quota file.
****************************************************************************/

static void reply_ntcreate_and_X_quota(connection_struct *conn,
				       struct smb_request *req,
				       enum FAKE_FILE_TYPE fake_file_type,
				       const char *fname)
{
	char *p;
	uint32 desired_access = IVAL(req->inbuf,smb_ntcreate_DesiredAccess);
	files_struct *fsp;
	NTSTATUS status;

	status = open_fake_file(conn, fake_file_type, fname, desired_access,
				&fsp);

	if (!NT_STATUS_IS_OK(status)) {
		reply_nterror(req, status);
		return;
	}

	reply_outbuf(req, 34, 0);

	p = (char *)req->outbuf + smb_vwv2;

	/* SCVAL(p,0,NO_OPLOCK_RETURN); */
	p++;
	SSVAL(p,0,fsp->fnum);

	DEBUG(5,("reply_ntcreate_and_X_quota: fnum = %d, open name = %s\n", fsp->fnum, fsp->fsp_name));

	chain_reply(req);
}

/****************************************************************************
 Reply to an NT create and X call.
****************************************************************************/

void reply_ntcreate_and_X(connection_struct *conn,
			  struct smb_request *req)
{
	char *fname = NULL;
	uint32 flags;
	uint32 access_mask;
	uint32 file_attributes;
	uint32 share_access;
	uint32 create_disposition;
	uint32 create_options;
	uint16 root_dir_fid;
	SMB_BIG_UINT allocation_size;
	/* Breakout the oplock request bits so we can set the
	   reply bits separately. */
	int oplock_request = 0;
	uint32 fattr=0;
	SMB_OFF_T file_len = 0;
	SMB_STRUCT_STAT sbuf;
	int info = 0;
	files_struct *fsp = NULL;
	char *p = NULL;
	struct timespec c_timespec;
	struct timespec a_timespec;
	struct timespec m_timespec;
	bool extended_oplock_granted = False;
	NTSTATUS status;
	struct case_semantics_state *case_state = NULL;
	TALLOC_CTX *ctx = talloc_tos();

	START_PROFILE(SMBntcreateX);

	if (req->wct < 24) {
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return;
	}

	flags = IVAL(req->inbuf,smb_ntcreate_Flags);
	access_mask = IVAL(req->inbuf,smb_ntcreate_DesiredAccess);
	file_attributes = IVAL(req->inbuf,smb_ntcreate_FileAttributes);
	share_access = IVAL(req->inbuf,smb_ntcreate_ShareAccess);
	create_disposition = IVAL(req->inbuf,smb_ntcreate_CreateDisposition);
	create_options = IVAL(req->inbuf,smb_ntcreate_CreateOptions);
	root_dir_fid = (uint16)IVAL(req->inbuf,smb_ntcreate_RootDirectoryFid);

	allocation_size = (SMB_BIG_UINT)IVAL(req->inbuf,smb_ntcreate_AllocationSize);
#ifdef LARGE_SMB_OFF_T
	allocation_size |= (((SMB_BIG_UINT)IVAL(req->inbuf,smb_ntcreate_AllocationSize + 4)) << 32);
#endif

	DEBUG(10,("reply_ntcreate_and_X: flags = 0x%x, access_mask = 0x%x "
		  "file_attributes = 0x%x, share_access = 0x%x, "
		  "create_disposition = 0x%x create_options = 0x%x "
		  "root_dir_fid = 0x%x\n",
			(unsigned int)flags,
			(unsigned int)access_mask,
			(unsigned int)file_attributes,
			(unsigned int)share_access,
			(unsigned int)create_disposition,
			(unsigned int)create_options,
			(unsigned int)root_dir_fid ));

	/*
	 * If it's an IPC, use the pipe handler.
	 */

	if (IS_IPC(conn)) {
		if (lp_nt_pipe_support()) {
			do_ntcreate_pipe_open(conn, req);
			END_PROFILE(SMBntcreateX);
			return;
		} else {
			reply_doserror(req, ERRDOS, ERRnoaccess);
			END_PROFILE(SMBntcreateX);
			return;
		}
	}

	if (create_options & FILE_OPEN_BY_FILE_ID) {
		reply_nterror(req, NT_STATUS_NOT_SUPPORTED);
		END_PROFILE(SMBntcreateX);
		return;
	}

	/*
	 * Get the file name.
	 */

	if(root_dir_fid != 0) {
		/*
		 * This filename is relative to a directory fid.
		 */
		char *rel_fname = NULL;
		files_struct *dir_fsp = file_fsp(
			SVAL(req->inbuf, smb_ntcreate_RootDirectoryFid));

		if(!dir_fsp) {
			reply_doserror(req, ERRDOS, ERRbadfid);
			END_PROFILE(SMBntcreateX);
			return;
		}

		if(!dir_fsp->is_directory) {

			srvstr_get_path(ctx, (char *)req->inbuf,
					req->flags2, &fname,
					smb_buf(req->inbuf), 0,
					STR_TERMINATE, &status);
			if (!NT_STATUS_IS_OK(status)) {
				reply_nterror(req, status);
				END_PROFILE(SMBntcreateX);
				return;
			}

			/*
			 * Check to see if this is a mac fork of some kind.
			 */

			if( is_ntfs_stream_name(fname)) {
				reply_nterror(
					req, NT_STATUS_OBJECT_PATH_NOT_FOUND);
				END_PROFILE(SMBntcreateX);
				return;
			}

			/*
			  we need to handle the case when we get a
			  relative open relative to a file and the
			  pathname is blank - this is a reopen!
			  (hint from demyn plantenberg)
			*/

			reply_doserror(req, ERRDOS, ERRbadfid);
			END_PROFILE(SMBntcreateX);
			return;
		}

		if (ISDOT(dir_fsp->fsp_name)) {
			/*
			 * We're at the toplevel dir, the final file name
			 * must not contain ./, as this is filtered out
			 * normally by srvstr_get_path and unix_convert
			 * explicitly rejects paths containing ./.
			 */
			fname = talloc_strdup(ctx,"");
			if (!fname) {
				reply_nterror(req, NT_STATUS_NO_MEMORY);
				END_PROFILE(SMBntcreateX);
				return;
			}
		} else {
			size_t dir_name_len = strlen(dir_fsp->fsp_name);

			/*
			 * Copy in the base directory name.
			 */

			fname = TALLOC_ARRAY(ctx, char, dir_name_len+2);
			if (!fname) {
				reply_nterror(
					req, NT_STATUS_NO_MEMORY);
				END_PROFILE(SMBntcreateX);
				return;
			}
			memcpy(fname, dir_fsp->fsp_name, dir_name_len+1);

			/*
			 * Ensure it ends in a '/'.
			 * We used TALLOC_SIZE +2 to add space for the '/'.
			 */

			if(dir_name_len &&
					(fname[dir_name_len-1] != '\\') &&
					(fname[dir_name_len-1] != '/')) {
				fname[dir_name_len] = '/';
				fname[dir_name_len+1] = '\0';
			}
		}

		srvstr_get_path(ctx, (char *)req->inbuf, req->flags2, &rel_fname,
				smb_buf(req->inbuf), 0,
				STR_TERMINATE, &status);
		if (!NT_STATUS_IS_OK(status)) {
			reply_nterror(req, status);
			END_PROFILE(SMBntcreateX);
			return;
		}
		fname = talloc_asprintf(ctx, "%s%s",
					fname,
					rel_fname);
		if (!fname) {
			reply_nterror(
				req, NT_STATUS_NO_MEMORY);
			END_PROFILE(SMBntcreateX);
			return;
		}
	} else {
		srvstr_get_path(ctx, (char *)req->inbuf, req->flags2, &fname,
				smb_buf(req->inbuf), 0,
				STR_TERMINATE, &status);
		if (!NT_STATUS_IS_OK(status)) {
			reply_nterror(req, status);
			END_PROFILE(SMBntcreateX);
			return;
		}

		/*
		 * Check to see if this is a mac fork of some kind.
		 */

		if( is_ntfs_stream_name(fname)) {
			enum FAKE_FILE_TYPE fake_file_type = is_fake_file(fname);
			if (fake_file_type!=FAKE_FILE_TYPE_NONE) {
				/*
				 * Here we go! support for changing the disk quotas --metze
				 *
				 * We need to fake up to open this MAGIC QUOTA file 
				 * and return a valid FID.
				 *
				 * w2k close this file directly after openening
				 * xp also tries a QUERY_FILE_INFO on the file and then close it
				 */
				reply_ntcreate_and_X_quota(conn, req,
							  fake_file_type, fname);
			} else {
				reply_nterror(req, NT_STATUS_OBJECT_PATH_NOT_FOUND);
			}
			END_PROFILE(SMBntcreateX);
			return;
		}
	}

	/*
	 * Now contruct the smb_open_mode value from the filename,
	 * desired access and the share access.
	 */
	status = resolve_dfspath(ctx, conn,
				req->flags2 & FLAGS2_DFS_PATHNAMES,
				fname,
				&fname);
	if (!NT_STATUS_IS_OK(status)) {
		if (NT_STATUS_EQUAL(status,NT_STATUS_PATH_NOT_COVERED)) {
			reply_botherror(req, NT_STATUS_PATH_NOT_COVERED,
					ERRSRV, ERRbadpath);
		}
		else {
			reply_nterror(req, status);
		}
		END_PROFILE(SMBntcreateX);
		return;
	}

	oplock_request = (flags & REQUEST_OPLOCK) ? EXCLUSIVE_OPLOCK : 0;
	if (oplock_request) {
		oplock_request |= (flags & REQUEST_BATCH_OPLOCK) ? BATCH_OPLOCK : 0;
	}

	/*
	 * Ordinary file or directory.
	 */

	/*
	 * Check if POSIX semantics are wanted.
	 */

	if (file_attributes & FILE_FLAG_POSIX_SEMANTICS) {
		case_state = set_posix_case_semantics(NULL, conn);
		file_attributes &= ~FILE_FLAG_POSIX_SEMANTICS;
	}

	status = unix_convert(ctx, conn, fname, False, &fname, NULL, &sbuf);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(case_state);
		reply_nterror(req, status);
		END_PROFILE(SMBntcreateX);
		return;
	}
	/* All file access must go through check_name() */
	status = check_name(conn, fname);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(case_state);
		reply_nterror(req, status);
		END_PROFILE(SMBntcreateX);
		return;
	}

	/* This is the correct thing to do (check every time) but can_delete is
	   expensive (it may have to read the parent directory permissions). So
	   for now we're not doing it unless we have a strong hint the client
	   is really going to delete this file. If the client is forcing FILE_CREATE
	   let the filesystem take care of the permissions. */

	/* Setting FILE_SHARE_DELETE is the hint. */

	if (lp_acl_check_permissions(SNUM(conn))
	    && (create_disposition != FILE_CREATE)
	    && (share_access & FILE_SHARE_DELETE)
	    && (access_mask & DELETE_ACCESS)) {
		if ((dos_mode(conn, fname, &sbuf) & FILE_ATTRIBUTE_READONLY) ||
				!can_delete_file_in_directory(conn, fname)) {
			TALLOC_FREE(case_state);
			reply_nterror(req, NT_STATUS_ACCESS_DENIED);
			END_PROFILE(SMBntcreateX);
			return;
		}
	}

#if 0
	/* We need to support SeSecurityPrivilege for this. */
	if ((access_mask & SEC_RIGHT_SYSTEM_SECURITY) &&
			!user_has_privileges(current_user.nt_user_token,
				&se_security)) {
		TALLOC_FREE(case_state);
		END_PROFILE(SMBntcreateX);
		return ERROR_NT(NT_STATUS_PRIVILEGE_NOT_HELD);
	}
#endif

	/*
	 * If it's a request for a directory open, deal with it separately.
	 */

	if(create_options & FILE_DIRECTORY_FILE) {

		/* Can't open a temp directory. IFS kit test. */
		if (file_attributes & FILE_ATTRIBUTE_TEMPORARY) {
			TALLOC_FREE(case_state);
			reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
			END_PROFILE(SMBntcreateX);
			return;
		}

		oplock_request = 0;
		status = open_directory(conn, req, fname, &sbuf,
					access_mask,
					share_access,
					create_disposition,
					create_options,
					file_attributes,
					&info, &fsp);

	} else {

		/*
		 * Ordinary file case.
		 */

		/* NB. We have a potential bug here. If we
		 * cause an oplock break to ourselves, then we
		 * could end up processing filename related
		 * SMB requests whilst we await the oplock
		 * break response. As we may have changed the
		 * filename case semantics to be POSIX-like,
		 * this could mean a filename request could
		 * fail when it should succeed. This is a rare
		 * condition, but eventually we must arrange
		 * to restore the correct case semantics
		 * before issuing an oplock break request to
		 * our client. JRA.  */

		status = open_file_ntcreate(conn, req, fname, &sbuf,
					access_mask,
					share_access,
					create_disposition,
					create_options,
					file_attributes,
					oplock_request,
					&info, &fsp);

		/* We cheat here. There are two cases we
		 * care about. One is a directory rename,
		 * where the NT client will attempt to
		 * open the source directory for
		 * DELETE access. Note that when the
		 * NT client does this it does *not*
		 * set the directory bit in the
		 * request packet. This is translated
		 * into a read/write open
		 * request. POSIX states that any open
		 * for write request on a directory
		 * will generate an EISDIR error, so
		 * we can catch this here and open a
		 * pseudo handle that is flagged as a
		 * directory. The second is an open
		 * for a permissions read only, which
		 * we handle in the open_file_stat case. JRA.
		 */

		if (NT_STATUS_EQUAL(status, NT_STATUS_FILE_IS_A_DIRECTORY)) {

			/*
			 * Fail the open if it was explicitly a non-directory
			 * file.
			 */

			if (create_options & FILE_NON_DIRECTORY_FILE) {
				TALLOC_FREE(case_state);
				reply_force_nterror(req,
						NT_STATUS_FILE_IS_A_DIRECTORY);
				END_PROFILE(SMBntcreateX);
				return;
			}

			oplock_request = 0;
			status = open_directory(conn, req, fname,
						&sbuf,
						access_mask,
						share_access,
						create_disposition,
						create_options,
						file_attributes,
						&info, &fsp);
		}
	}

	TALLOC_FREE(case_state);

	if (!NT_STATUS_IS_OK(status)) {
		if (open_was_deferred(req->mid)) {
			/* We have re-scheduled this call. */
			END_PROFILE(SMBntcreateX);
			return;
		}
		reply_openerror(req, status);
		END_PROFILE(SMBntcreateX);
		return;
	}

	file_len = sbuf.st_size;
	fattr = dos_mode(conn,fname,&sbuf);
	if(fattr == 0) {
		fattr = FILE_ATTRIBUTE_NORMAL;
	}
	if (!fsp->is_directory && (fattr & aDIR)) {
		close_file(fsp,ERROR_CLOSE);
		reply_doserror(req, ERRDOS, ERRnoaccess);
		END_PROFILE(SMBntcreateX);
		return;
	}

	/* Save the requested allocation size. */
	if ((info == FILE_WAS_CREATED) || (info == FILE_WAS_OVERWRITTEN)) {
		if (allocation_size && (allocation_size > (SMB_BIG_UINT)file_len)) {
			fsp->initial_allocation_size = smb_roundup(fsp->conn, allocation_size);
			if (fsp->is_directory) {
				close_file(fsp,ERROR_CLOSE);
				/* Can't set allocation size on a directory. */
				reply_nterror(req, NT_STATUS_ACCESS_DENIED);
				END_PROFILE(SMBntcreateX);
				return;
			}
			if (vfs_allocate_file_space(fsp, fsp->initial_allocation_size) == -1) {
				close_file(fsp,ERROR_CLOSE);
				reply_nterror(req, NT_STATUS_DISK_FULL);
				END_PROFILE(SMBntcreateX);
				return;
			}
		} else {
			fsp->initial_allocation_size = smb_roundup(fsp->conn,(SMB_BIG_UINT)file_len);
		}
	}

	/*
	 * If the caller set the extended oplock request bit
	 * and we granted one (by whatever means) - set the
	 * correct bit for extended oplock reply.
	 */

	if (oplock_request && lp_fake_oplocks(SNUM(conn))) {
		extended_oplock_granted = True;
	}

	if(oplock_request && EXCLUSIVE_OPLOCK_TYPE(fsp->oplock_type)) {
		extended_oplock_granted = True;
	}

	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		/* This is very strange. We
 		 * return 50 words, but only set
 		 * the wcnt to 42 ? It's definately
 		 * what happens on the wire....
 		 */
		reply_outbuf(req, 50, 0);
		SCVAL(req->outbuf,smb_wct,42);
	} else {
		reply_outbuf(req, 34, 0);
	}

	p = (char *)req->outbuf + smb_vwv2;

	/*
	 * Currently as we don't support level II oplocks we just report
	 * exclusive & batch here.
	 */

	if (extended_oplock_granted) {
		if (flags & REQUEST_BATCH_OPLOCK) {
			SCVAL(p,0, BATCH_OPLOCK_RETURN);
		} else {
			SCVAL(p,0, EXCLUSIVE_OPLOCK_RETURN);
		}
	} else if (fsp->oplock_type == LEVEL_II_OPLOCK) {
		SCVAL(p,0, LEVEL_II_OPLOCK_RETURN);
	} else {
		SCVAL(p,0,NO_OPLOCK_RETURN);
	}

	p++;
	SSVAL(p,0,fsp->fnum);
	p += 2;
	if ((create_disposition == FILE_SUPERSEDE) && (info == FILE_WAS_OVERWRITTEN)) {
		SIVAL(p,0,FILE_WAS_SUPERSEDED);
	} else {
		SIVAL(p,0,info);
	}
	p += 4;

	/* Create time. */
	c_timespec = get_create_timespec(&sbuf,lp_fake_dir_create_times(SNUM(conn)));
	a_timespec = get_atimespec(&sbuf);
	m_timespec = get_mtimespec(&sbuf);

	if (lp_dos_filetime_resolution(SNUM(conn))) {
		dos_filetime_timespec(&c_timespec);
		dos_filetime_timespec(&a_timespec);
		dos_filetime_timespec(&m_timespec);
	}

	put_long_date_timespec(p, c_timespec); /* create time. */
	p += 8;
	put_long_date_timespec(p, a_timespec); /* access time */
	p += 8;
	put_long_date_timespec(p, m_timespec); /* write time */
	p += 8;
	put_long_date_timespec(p, m_timespec); /* change time */
	p += 8;
	SIVAL(p,0,fattr); /* File Attributes. */
	p += 4;
	SOFF_T(p, 0, get_allocation_size(conn,fsp,&sbuf));
	p += 8;
	SOFF_T(p,0,file_len);
	p += 8;
	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		SSVAL(p,2,0x7);
	}
	p += 4;
	SCVAL(p,0,fsp->is_directory ? 1 : 0);

	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		uint32 perms = 0;
		p += 25;
		if (fsp->is_directory || can_write_to_file(conn, fname, &sbuf)) {
			perms = FILE_GENERIC_ALL;
		} else {
			perms = FILE_GENERIC_READ|FILE_EXECUTE;
		}
		SIVAL(p,0,perms);
	}

	DEBUG(5,("reply_ntcreate_and_X: fnum = %d, open name = %s\n", fsp->fnum, fsp->fsp_name));

	chain_reply(req);
	END_PROFILE(SMBntcreateX);
	return;
}

/****************************************************************************
 Reply to a NT_TRANSACT_CREATE call to open a pipe.
****************************************************************************/

static void do_nt_transact_create_pipe(connection_struct *conn,
				       struct smb_request *req,
				       uint16 **ppsetup, uint32 setup_count,
				       char **ppparams, uint32 parameter_count,
				       char **ppdata, uint32 data_count)
{
	char *fname = NULL;
	char *params = *ppparams;
	int pnum = -1;
	char *p = NULL;
	NTSTATUS status;
	size_t param_len;
	uint32 flags;
	TALLOC_CTX *ctx = talloc_tos();

	/*
	 * Ensure minimum number of parameters sent.
	 */

	if(parameter_count < 54) {
		DEBUG(0,("do_nt_transact_create_pipe - insufficient parameters (%u)\n", (unsigned int)parameter_count));
		reply_doserror(req, ERRDOS, ERRnoaccess);
		return;
	}

	flags = IVAL(params,0);

	srvstr_get_path(ctx, params, req->flags2, &fname, params+53,
			parameter_count-53, STR_TERMINATE,
			&status);
	if (!NT_STATUS_IS_OK(status)) {
		reply_nterror(req, status);
		return;
	}

	nt_open_pipe(fname, conn, req, &pnum);

	if (req->outbuf) {
		/* Error return */
		return;
	}

	/* Realloc the size of parameters and data we will return */
	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		/* Extended response is 32 more byyes. */
		param_len = 101;
	} else {
		param_len = 69;
	}
	params = nttrans_realloc(ppparams, param_len);
	if(params == NULL) {
		reply_doserror(req, ERRDOS, ERRnomem);
		return;
	}

	p = params;
	SCVAL(p,0,NO_OPLOCK_RETURN);

	p += 2;
	SSVAL(p,0,pnum);
	p += 2;
	SIVAL(p,0,FILE_WAS_OPENED);
	p += 8;

	p += 32;
	SIVAL(p,0,FILE_ATTRIBUTE_NORMAL); /* File Attributes. */
	p += 20;
	/* File type. */
	SSVAL(p,0,FILE_TYPE_MESSAGE_MODE_PIPE);
	/* Device state. */
	SSVAL(p,2, 0x5FF); /* ? */
	p += 4;

	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		p += 25;
		SIVAL(p,0,FILE_GENERIC_ALL);
		/*
		 * For pipes W2K3 seems to return
 		 * 0x12019B next.
 		 * This is ((FILE_GENERIC_READ|FILE_GENERIC_WRITE) & ~FILE_APPEND_DATA)
 		 */
		SIVAL(p,4,(FILE_GENERIC_READ|FILE_GENERIC_WRITE)&~FILE_APPEND_DATA);
	}

	DEBUG(5,("do_nt_transact_create_pipe: open name = %s\n", fname));

	/* Send the required number of replies */
	send_nt_replies(req, NT_STATUS_OK, params, param_len, *ppdata, 0);

	return;
}

/****************************************************************************
 Internal fn to set security descriptors.
****************************************************************************/

static NTSTATUS set_sd(files_struct *fsp, char *data, uint32 sd_len, uint32 security_info_sent)
{
	prs_struct pd;
	SEC_DESC *psd = NULL;
	TALLOC_CTX *mem_ctx;
	NTSTATUS status;

	if (sd_len == 0 || !lp_nt_acl_support(SNUM(fsp->conn))) {
		return NT_STATUS_OK;
	}

	/*
	 * Init the parse struct we will unmarshall from.
	 */

	if ((mem_ctx = talloc_init("set_sd")) == NULL) {
		DEBUG(0,("set_sd: talloc_init failed.\n"));
		return NT_STATUS_NO_MEMORY;
	}

	prs_init(&pd, 0, mem_ctx, UNMARSHALL);

	/*
	 * Setup the prs_struct to point at the memory we just
	 * allocated.
	 */

	prs_give_memory( &pd, data, sd_len, False);

	/*
	 * Finally, unmarshall from the data buffer.
	 */

	if(!sec_io_desc( "sd data", &psd, &pd, 1)) {
		DEBUG(0,("set_sd: Error in unmarshalling security descriptor.\n"));
		/*
		 * Return access denied for want of a better error message..
		 */
		talloc_destroy(mem_ctx);
		return NT_STATUS_NO_MEMORY;
	}

	if (psd->owner_sid==0) {
		security_info_sent &= ~OWNER_SECURITY_INFORMATION;
	}
	if (psd->group_sid==0) {
		security_info_sent &= ~GROUP_SECURITY_INFORMATION;
	}
	if (psd->sacl==0) {
		security_info_sent &= ~SACL_SECURITY_INFORMATION;
	}
	if (psd->dacl==0) {
		security_info_sent &= ~DACL_SECURITY_INFORMATION;
	}

	status = SMB_VFS_FSET_NT_ACL( fsp, fsp->fh->fd, security_info_sent, psd);

	talloc_destroy(mem_ctx);
	return status;
}

/****************************************************************************
 Read a list of EA names and data from an incoming data buffer. Create an ea_list with them.
****************************************************************************/

static struct ea_list *read_nttrans_ea_list(TALLOC_CTX *ctx, const char *pdata, size_t data_size)
{
	struct ea_list *ea_list_head = NULL;
	size_t offset = 0;

	if (data_size < 4) {
		return NULL;
	}

	while (offset + 4 <= data_size) {
		size_t next_offset = IVAL(pdata,offset);
		struct ea_list *eal = read_ea_list_entry(ctx, pdata + offset + 4, data_size - offset - 4, NULL);

		if (!eal) {
			return NULL;
		}

		DLIST_ADD_END(ea_list_head, eal, struct ea_list *);
		if (next_offset == 0) {
			break;
		}
		offset += next_offset;
	}

	return ea_list_head;
}

/****************************************************************************
 Reply to a NT_TRANSACT_CREATE call (needs to process SD's).
****************************************************************************/

static void call_nt_transact_create(connection_struct *conn,
				    struct smb_request *req,
				    uint16 **ppsetup, uint32 setup_count,
				    char **ppparams, uint32 parameter_count,
				    char **ppdata, uint32 data_count,
				    uint32 max_data_count)
{
	char *fname = NULL;
	char *params = *ppparams;
	char *data = *ppdata;
	/* Breakout the oplock request bits so we can set the reply bits separately. */
	int oplock_request = 0;
	uint32 fattr=0;
	SMB_OFF_T file_len = 0;
	SMB_STRUCT_STAT sbuf;
	int info = 0;
	files_struct *fsp = NULL;
	char *p = NULL;
	bool extended_oplock_granted = False;
	uint32 flags;
	uint32 access_mask;
	uint32 file_attributes;
	uint32 share_access;
	uint32 create_disposition;
	uint32 create_options;
	uint32 sd_len;
	uint32 ea_len;
	uint16 root_dir_fid;
	struct timespec c_timespec;
	struct timespec a_timespec;
	struct timespec m_timespec;
	struct ea_list *ea_list = NULL;
	char *pdata = NULL;
	NTSTATUS status;
	size_t param_len;
	struct case_semantics_state *case_state = NULL;
	TALLOC_CTX *ctx = talloc_tos();

	DEBUG(5,("call_nt_transact_create\n"));

	/*
	 * If it's an IPC, use the pipe handler.
	 */

	if (IS_IPC(conn)) {
		if (lp_nt_pipe_support()) {
			do_nt_transact_create_pipe(
				conn, req,
				ppsetup, setup_count,
				ppparams, parameter_count,
				ppdata, data_count);
			return;
		} else {
			reply_doserror(req, ERRDOS, ERRnoaccess);
			return;
		}
	}

	/*
	 * Ensure minimum number of parameters sent.
	 */

	if(parameter_count < 54) {
		DEBUG(0,("call_nt_transact_create - insufficient parameters (%u)\n", (unsigned int)parameter_count));
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return;
	}

	flags = IVAL(params,0);
	access_mask = IVAL(params,8);
	file_attributes = IVAL(params,20);
	share_access = IVAL(params,24);
	create_disposition = IVAL(params,28);
	create_options = IVAL(params,32);
	sd_len = IVAL(params,36);
	ea_len = IVAL(params,40);
	root_dir_fid = (uint16)IVAL(params,4);

	/* Ensure the data_len is correct for the sd and ea values given. */
	if ((ea_len + sd_len > data_count) ||
			(ea_len > data_count) || (sd_len > data_count) ||
			(ea_len + sd_len < ea_len) || (ea_len + sd_len < sd_len)) {
		DEBUG(10,("call_nt_transact_create - ea_len = %u, sd_len = %u, data_count = %u\n",
			(unsigned int)ea_len, (unsigned int)sd_len, (unsigned int)data_count ));
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return;
	}

	if (ea_len) {
		if (!lp_ea_support(SNUM(conn))) {
			DEBUG(10,("call_nt_transact_create - ea_len = %u but EA's not supported.\n",
				(unsigned int)ea_len ));
			reply_nterror(req, NT_STATUS_EAS_NOT_SUPPORTED);
			return;
		}

		if (ea_len < 10) {
			DEBUG(10,("call_nt_transact_create - ea_len = %u - too small (should be more than 10)\n",
				(unsigned int)ea_len ));
			reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
			return;
		}
	}

	if (create_options & FILE_OPEN_BY_FILE_ID) {
		reply_nterror(req, NT_STATUS_NOT_SUPPORTED);
		return;
	}

	/*
	 * Get the file name.
	 */

	if(root_dir_fid != 0) {
		/*
		 * This filename is relative to a directory fid.
		 */
		char *tmpname = NULL;
		files_struct *dir_fsp = file_fsp(SVAL(params,4));

		if(!dir_fsp) {
			reply_doserror(req, ERRDOS, ERRbadfid);
			return;
		}

		if(!dir_fsp->is_directory) {
			srvstr_get_path(ctx, params, req->flags2, &fname,
					params+53,
					parameter_count-53, STR_TERMINATE,
					&status);
			if (!NT_STATUS_IS_OK(status)) {
				reply_nterror(req, status);
				return;
			}

			/*
			 * Check to see if this is a mac fork of some kind.
			 */

			if( is_ntfs_stream_name(fname)) {
				reply_nterror(req,
					      NT_STATUS_OBJECT_PATH_NOT_FOUND);
				return;
			}

			reply_doserror(req, ERRDOS, ERRbadfid);
			return;
		}

		if (ISDOT(dir_fsp->fsp_name)) {
			/*
			 * We're at the toplevel dir, the final file name
			 * must not contain ./, as this is filtered out
			 * normally by srvstr_get_path and unix_convert
			 * explicitly rejects paths containing ./.
			 */
			fname = talloc_strdup(ctx,"");
			if (!fname) {
				reply_nterror(req, NT_STATUS_NO_MEMORY);
				return;
			}
		} else {
			size_t dir_name_len = strlen(dir_fsp->fsp_name);

			/*
			 * Copy in the base directory name.
			 */

			fname = TALLOC_ARRAY(ctx, char, dir_name_len+2);
			if (!fname) {
				reply_nterror(req, NT_STATUS_NO_MEMORY);
				return;
			}
			memcpy(fname, dir_fsp->fsp_name, dir_name_len+1);

			/*
			 * Ensure it ends in a '/'.
			 * We used TALLOC_SIZE +2 to add space for the '/'.
			 */

			if(dir_name_len &&
					(fname[dir_name_len-1] != '\\') &&
					(fname[dir_name_len-1] != '/')) {
				fname[dir_name_len] = '/';
				fname[dir_name_len+1] = '\0';
			}
		}

		srvstr_get_path(ctx, params, req->flags2, &tmpname,
				params+53,
				parameter_count-53, STR_TERMINATE,
				&status);
		if (!NT_STATUS_IS_OK(status)) {
			reply_nterror(req, status);
			return;
		}
		fname = talloc_asprintf(ctx, "%s%s",
					fname,
					tmpname);
		if (!fname) {
			reply_nterror(
				req, NT_STATUS_NO_MEMORY);
			return;
		}
	} else {
		srvstr_get_path(ctx, params, req->flags2, &fname, params+53,
				parameter_count-53,
				STR_TERMINATE, &status);
		if (!NT_STATUS_IS_OK(status)) {
			reply_nterror(req, status);
			return;
		}

		/*
		 * Check to see if this is a mac fork of some kind.
		 */

		if( is_ntfs_stream_name(fname)) {
			reply_nterror(req, NT_STATUS_OBJECT_PATH_NOT_FOUND);
			return;
		}
	}

	oplock_request = (flags & REQUEST_OPLOCK) ? EXCLUSIVE_OPLOCK : 0;
	if (oplock_request) {
		oplock_request |= (flags & REQUEST_BATCH_OPLOCK) ? BATCH_OPLOCK : 0;
	}

	/*
	 * Ordinary file or directory.
	 */

	/*
	 * Check if POSIX semantics are wanted.
	 */

	if (file_attributes & FILE_FLAG_POSIX_SEMANTICS) {
		case_state = set_posix_case_semantics(NULL, conn);
		file_attributes &= ~FILE_FLAG_POSIX_SEMANTICS;
	}

	status = resolve_dfspath(ctx, conn,
				req->flags2 & FLAGS2_DFS_PATHNAMES,
				fname,
				&fname);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(case_state);
		if (NT_STATUS_EQUAL(status,NT_STATUS_PATH_NOT_COVERED)) {
			reply_botherror(req, NT_STATUS_PATH_NOT_COVERED,
					ERRSRV, ERRbadpath);
			return;
		}
		reply_nterror(req, status);
		return;
	}

	status = unix_convert(ctx, conn, fname, False, &fname, NULL, &sbuf);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(case_state);
		reply_nterror(req, status);
		return;
	}
	/* All file access must go through check_name() */
	status = check_name(conn, fname);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(case_state);
		reply_nterror(req, status);
		return;
	}

	/* This is the correct thing to do (check every time) but can_delete is
	   expensive (it may have to read the parent directory permissions). So
	   for now we're not doing it unless we have a strong hint the client
	   is really going to delete this file. If the client is forcing FILE_CREATE
	   let the filesystem take care of the permissions. */

	/* Setting FILE_SHARE_DELETE is the hint. */

	if (lp_acl_check_permissions(SNUM(conn))
	    && (create_disposition != FILE_CREATE)
	    && (share_access & FILE_SHARE_DELETE)
	    && (access_mask & DELETE_ACCESS)) {
		if ((dos_mode(conn, fname, &sbuf) & FILE_ATTRIBUTE_READONLY) ||
				!can_delete_file_in_directory(conn, fname)) {
			TALLOC_FREE(case_state);
			reply_nterror(req, NT_STATUS_ACCESS_DENIED);
			return;
		}
	}

#if 0
	/* We need to support SeSecurityPrivilege for this. */
	if ((access_mask & SEC_RIGHT_SYSTEM_SECURITY) &&
			!user_has_privileges(current_user.nt_user_token,
				&se_security)) {
		TALLOC_FREE(case_state);
		reply_nterror(req, NT_STATUS_PRIVILEGE_NOT_HELD);
		return;
	}
#endif

	if (ea_len) {
		pdata = data + sd_len;

		/* We have already checked that ea_len <= data_count here. */
		ea_list = read_nttrans_ea_list(talloc_tos(), pdata,
					       ea_len);
		if (!ea_list ) {
			TALLOC_FREE(case_state);
			reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
			return;
		}
	}

	/*
	 * If it's a request for a directory open, deal with it separately.
	 */

	if(create_options & FILE_DIRECTORY_FILE) {

		/* Can't open a temp directory. IFS kit test. */
		if (file_attributes & FILE_ATTRIBUTE_TEMPORARY) {
			TALLOC_FREE(case_state);
			reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
			return;
		}

		/*
		 * We will get a create directory here if the Win32
		 * app specified a security descriptor in the 
		 * CreateDirectory() call.
		 */

		oplock_request = 0;
		status = open_directory(conn, req, fname, &sbuf,
					access_mask,
					share_access,
					create_disposition,
					create_options,
					file_attributes,
					&info, &fsp);
	} else {

		/*
		 * Ordinary file case.
		 */

		status = open_file_ntcreate(conn,req,fname,&sbuf,
					access_mask,
					share_access,
					create_disposition,
					create_options,
					file_attributes,
					oplock_request,
					&info, &fsp);

		if (NT_STATUS_EQUAL(status, NT_STATUS_FILE_IS_A_DIRECTORY)) {

			/*
			 * Fail the open if it was explicitly a non-directory file.
			 */

			if (create_options & FILE_NON_DIRECTORY_FILE) {
				TALLOC_FREE(case_state);
				reply_force_nterror(
					req,
					NT_STATUS_FILE_IS_A_DIRECTORY);
				return;
			}

			oplock_request = 0;
			status = open_directory(conn, req, fname,
						&sbuf,
						access_mask,
						share_access,
						create_disposition,
						create_options,
						file_attributes,
						&info, &fsp);
		}
	}

	TALLOC_FREE(case_state);

	if(!NT_STATUS_IS_OK(status)) {
		if (open_was_deferred(req->mid)) {
			/* We have re-scheduled this call. */
			return;
		}
		reply_openerror(req, status);
		return;
	}

	/*
	 * According to the MS documentation, the only time the security
	 * descriptor is applied to the opened file is iff we *created* the
	 * file; an existing file stays the same.
	 *
	 * Also, it seems (from observation) that you can open the file with
	 * any access mask but you can still write the sd. We need to override
	 * the granted access before we call set_sd
	 * Patch for bug #2242 from Tom Lackemann <cessnatomny@yahoo.com>.
	 */

	if (lp_nt_acl_support(SNUM(conn)) && sd_len && info == FILE_WAS_CREATED) {
		uint32 saved_access_mask = fsp->access_mask;

		/* We have already checked that sd_len <= data_count here. */

		fsp->access_mask = FILE_GENERIC_ALL;

		status = set_sd( fsp, data, sd_len, ALL_SECURITY_INFORMATION);
		if (!NT_STATUS_IS_OK(status)) {
			close_file(fsp,ERROR_CLOSE);
			TALLOC_FREE(case_state);
			reply_nterror(req, status);
			return;
		}
		fsp->access_mask = saved_access_mask;
 	}

	if (ea_len && (info == FILE_WAS_CREATED)) {
		status = set_ea(conn, fsp, fname, ea_list);
		if (!NT_STATUS_IS_OK(status)) {
			close_file(fsp,ERROR_CLOSE);
			TALLOC_FREE(case_state);
			reply_nterror(req, status);
			return;
		}
	}

	TALLOC_FREE(case_state);

	file_len = sbuf.st_size;
	fattr = dos_mode(conn,fname,&sbuf);
	if(fattr == 0) {
		fattr = FILE_ATTRIBUTE_NORMAL;
	}
	if (!fsp->is_directory && (fattr & aDIR)) {
		close_file(fsp,ERROR_CLOSE);
		reply_doserror(req, ERRDOS, ERRnoaccess);
		return;
	}

	/* Save the requested allocation size. */
	if ((info == FILE_WAS_CREATED) || (info == FILE_WAS_OVERWRITTEN)) {
		SMB_BIG_UINT allocation_size = (SMB_BIG_UINT)IVAL(params,12);
#ifdef LARGE_SMB_OFF_T
		allocation_size |= (((SMB_BIG_UINT)IVAL(params,16)) << 32);
#endif
		if (allocation_size && (allocation_size > file_len)) {
			fsp->initial_allocation_size = smb_roundup(fsp->conn, allocation_size);
			if (fsp->is_directory) {
				close_file(fsp,ERROR_CLOSE);
				/* Can't set allocation size on a directory. */
				reply_nterror(req, NT_STATUS_ACCESS_DENIED);
				return;
			}
			if (vfs_allocate_file_space(fsp, fsp->initial_allocation_size) == -1) {
				close_file(fsp,ERROR_CLOSE);
				reply_nterror(req, NT_STATUS_DISK_FULL);
				return;
			}
		} else {
			fsp->initial_allocation_size = smb_roundup(fsp->conn, (SMB_BIG_UINT)file_len);
		}
	}

	/*
	 * If the caller set the extended oplock request bit
	 * and we granted one (by whatever means) - set the
	 * correct bit for extended oplock reply.
	 */

	if (oplock_request && lp_fake_oplocks(SNUM(conn))) {
		extended_oplock_granted = True;
	}

	if(oplock_request && EXCLUSIVE_OPLOCK_TYPE(fsp->oplock_type)) {
		extended_oplock_granted = True;
	}

	/* Realloc the size of parameters and data we will return */
	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		/* Extended response is 32 more byyes. */
		param_len = 101;
	} else {
		param_len = 69;
	}
	params = nttrans_realloc(ppparams, param_len);
	if(params == NULL) {
		reply_doserror(req, ERRDOS, ERRnomem);
		return;
	}

	p = params;
	if (extended_oplock_granted) {
		if (flags & REQUEST_BATCH_OPLOCK) {
			SCVAL(p,0, BATCH_OPLOCK_RETURN);
		} else {
			SCVAL(p,0, EXCLUSIVE_OPLOCK_RETURN);
		}
	} else if (fsp->oplock_type == LEVEL_II_OPLOCK) {
		SCVAL(p,0, LEVEL_II_OPLOCK_RETURN);
	} else {
		SCVAL(p,0,NO_OPLOCK_RETURN);
	}

	p += 2;
	SSVAL(p,0,fsp->fnum);
	p += 2;
	if ((create_disposition == FILE_SUPERSEDE) && (info == FILE_WAS_OVERWRITTEN)) {
		SIVAL(p,0,FILE_WAS_SUPERSEDED);
	} else {
		SIVAL(p,0,info);
	}
	p += 8;

	/* Create time. */
	c_timespec = get_create_timespec(&sbuf,lp_fake_dir_create_times(SNUM(conn)));
	a_timespec = get_atimespec(&sbuf);
	m_timespec = get_mtimespec(&sbuf);

	if (lp_dos_filetime_resolution(SNUM(conn))) {
		dos_filetime_timespec(&c_timespec);
		dos_filetime_timespec(&a_timespec);
		dos_filetime_timespec(&m_timespec);
	}

	put_long_date_timespec(p, c_timespec); /* create time. */
	p += 8;
	put_long_date_timespec(p, a_timespec); /* access time */
	p += 8;
	put_long_date_timespec(p, m_timespec); /* write time */
	p += 8;
	put_long_date_timespec(p, m_timespec); /* change time */
	p += 8;
	SIVAL(p,0,fattr); /* File Attributes. */
	p += 4;
	SOFF_T(p, 0, get_allocation_size(conn,fsp,&sbuf));
	p += 8;
	SOFF_T(p,0,file_len);
	p += 8;
	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		SSVAL(p,2,0x7);
	}
	p += 4;
	SCVAL(p,0,fsp->is_directory ? 1 : 0);

	if (flags & EXTENDED_RESPONSE_REQUIRED) {
		uint32 perms = 0;
		p += 25;
		if (fsp->is_directory || can_write_to_file(conn, fname, &sbuf)) {
			perms = FILE_GENERIC_ALL;
		} else {
			perms = FILE_GENERIC_READ|FILE_EXECUTE;
		}
		SIVAL(p,0,perms);
	}

	DEBUG(5,("call_nt_transact_create: open name = %s\n", fname));

	/* Send the required number of replies */
	send_nt_replies(req, NT_STATUS_OK, params, param_len, *ppdata, 0);

	return;
}

/****************************************************************************
 Reply to a NT CANCEL request.
 conn POINTER CAN BE NULL HERE !
****************************************************************************/

void reply_ntcancel(connection_struct *conn, struct smb_request *req)
{
	/*
	 * Go through and cancel any pending change notifies.
	 */

	START_PROFILE(SMBntcancel);
	remove_pending_change_notify_requests_by_mid(req->mid);
	remove_pending_lock_requests_by_mid(req->mid);
	srv_cancel_sign_response(req->mid);

	DEBUG(3,("reply_ntcancel: cancel called on mid = %d.\n", req->mid));

	END_PROFILE(SMBntcancel);
	return;
}

/****************************************************************************
 Copy a file.
****************************************************************************/

static NTSTATUS copy_internals(TALLOC_CTX *ctx,
				connection_struct *conn,
				struct smb_request *req,
				const char *oldname_in,
				const char *newname_in,
				uint32 attrs)
{
	SMB_STRUCT_STAT sbuf1, sbuf2;
	char *oldname = NULL;
	char *newname = NULL;
	char *last_component_oldname = NULL;
	char *last_component_newname = NULL;
	files_struct *fsp1,*fsp2;
	uint32 fattr;
	int info;
	SMB_OFF_T ret=-1;
	NTSTATUS status = NT_STATUS_OK;

	ZERO_STRUCT(sbuf1);
	ZERO_STRUCT(sbuf2);

	if (!CAN_WRITE(conn)) {
		return NT_STATUS_MEDIA_WRITE_PROTECTED;
	}

	status = unix_convert(ctx, conn, oldname_in, False, &oldname,
			&last_component_oldname, &sbuf1);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	status = check_name(conn, oldname);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

        /* Source must already exist. */
	if (!VALID_STAT(sbuf1)) {
		return NT_STATUS_OBJECT_NAME_NOT_FOUND;
	}
	/* Ensure attributes match. */
	fattr = dos_mode(conn,oldname,&sbuf1);
	if ((fattr & ~attrs) & (aHIDDEN | aSYSTEM)) {
		return NT_STATUS_NO_SUCH_FILE;
	}

	status = unix_convert(ctx, conn, newname_in, False, &newname,
			&last_component_newname, &sbuf2);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	status = check_name(conn, newname);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* Disallow if newname already exists. */
	if (VALID_STAT(sbuf2)) {
		return NT_STATUS_OBJECT_NAME_COLLISION;
	}

	/* No links from a directory. */
	if (S_ISDIR(sbuf1.st_mode)) {
		return NT_STATUS_FILE_IS_A_DIRECTORY;
	}

	/* Ensure this is within the share. */
	status = check_reduced_name(conn, oldname);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(10,("copy_internals: doing file copy %s to %s\n",
				oldname, newname));

        status = open_file_ntcreate(conn, req, oldname, &sbuf1,
			FILE_READ_DATA, /* Read-only. */
			FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
			FILE_OPEN,
			0, /* No create options. */
			FILE_ATTRIBUTE_NORMAL,
			NO_OPLOCK,
			&info, &fsp1);

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

        status = open_file_ntcreate(conn, req, newname, &sbuf2,
			FILE_WRITE_DATA, /* Read-only. */
			FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
			FILE_CREATE,
			0, /* No create options. */
			fattr,
			NO_OPLOCK,
			&info, &fsp2);

	if (!NT_STATUS_IS_OK(status)) {
		close_file(fsp1,ERROR_CLOSE);
		return status;
	}

	if (sbuf1.st_size) {
		ret = vfs_transfer_file(fsp1, fsp2, sbuf1.st_size);
	}

	/*
	 * As we are opening fsp1 read-only we only expect
	 * an error on close on fsp2 if we are out of space.
	 * Thus we don't look at the error return from the
	 * close of fsp1.
	 */
	close_file(fsp1,NORMAL_CLOSE);

	/* Ensure the modtime is set correctly on the destination file. */
	fsp_set_pending_modtime(fsp2, get_mtimespec(&sbuf1));

	status = close_file(fsp2,NORMAL_CLOSE);

	/* Grrr. We have to do this as open_file_ntcreate adds aARCH when it
	   creates the file. This isn't the correct thing to do in the copy
	   case. JRA */
	file_set_dosmode(conn, newname, fattr, &sbuf2,
			 parent_dirname(newname));

	if (ret < (SMB_OFF_T)sbuf1.st_size) {
		return NT_STATUS_DISK_FULL;
	}

	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(3,("copy_internals: Error %s copy file %s to %s\n",
			nt_errstr(status), oldname, newname));
	}
	return status;
}

/****************************************************************************
 Reply to a NT rename request.
****************************************************************************/

void reply_ntrename(connection_struct *conn, struct smb_request *req)
{
	char *oldname = NULL;
	char *newname = NULL;
	char *p;
	NTSTATUS status;
	bool src_has_wcard = False;
	bool dest_has_wcard = False;
	uint32 attrs;
	uint16 rename_type;
	TALLOC_CTX *ctx = talloc_tos();

	START_PROFILE(SMBntrename);

	if (req->wct < 4) {
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		END_PROFILE(SMBntrename);
		return;
	}

	attrs = SVAL(req->inbuf,smb_vwv0);
	rename_type = SVAL(req->inbuf,smb_vwv1);

	p = smb_buf(req->inbuf) + 1;
	p += srvstr_get_path_wcard(ctx, (char *)req->inbuf, req->flags2, &oldname, p,
				   0, STR_TERMINATE, &status,
				   &src_has_wcard);
	if (!NT_STATUS_IS_OK(status)) {
		reply_nterror(req, status);
		END_PROFILE(SMBntrename);
		return;
	}

	if( is_ntfs_stream_name(oldname)) {
		/* Can't rename a stream. */
		reply_nterror(req, NT_STATUS_ACCESS_DENIED);
		END_PROFILE(SMBntrename);
		return;
	}

	if (ms_has_wild(oldname)) {
		reply_nterror(req, NT_STATUS_OBJECT_PATH_SYNTAX_BAD);
		END_PROFILE(SMBntrename);
		return;
	}

	p++;
	p += srvstr_get_path_wcard(ctx, (char *)req->inbuf, req->flags2, &newname, p,
				   0, STR_TERMINATE, &status,
				   &dest_has_wcard);
	if (!NT_STATUS_IS_OK(status)) {
		reply_nterror(req, status);
		END_PROFILE(SMBntrename);
		return;
	}

	status = resolve_dfspath(ctx, conn,
				req->flags2 & FLAGS2_DFS_PATHNAMES,
				oldname,
				&oldname);
	if (!NT_STATUS_IS_OK(status)) {
		if (NT_STATUS_EQUAL(status,NT_STATUS_PATH_NOT_COVERED)) {
			reply_botherror(req, NT_STATUS_PATH_NOT_COVERED,
					ERRSRV, ERRbadpath);
			END_PROFILE(SMBntrename);
			return;
		}
		reply_nterror(req, status);
		END_PROFILE(SMBntrename);
		return;
	}

	status = resolve_dfspath(ctx, conn,
				req->flags2 & FLAGS2_DFS_PATHNAMES,
				newname,
				&newname);
	if (!NT_STATUS_IS_OK(status)) {
		if (NT_STATUS_EQUAL(status,NT_STATUS_PATH_NOT_COVERED)) {
			reply_botherror(req, NT_STATUS_PATH_NOT_COVERED,
					ERRSRV, ERRbadpath);
			END_PROFILE(SMBntrename);
			return;
		}
		reply_nterror(req, status);
		END_PROFILE(SMBntrename);
		return;
	}

	DEBUG(3,("reply_ntrename : %s -> %s\n",oldname,newname));

	switch(rename_type) {
		case RENAME_FLAG_RENAME:
			status = rename_internals(ctx, conn, req, oldname,
					newname, attrs, False, src_has_wcard,
					dest_has_wcard);
			break;
		case RENAME_FLAG_HARD_LINK:
			if (src_has_wcard || dest_has_wcard) {
				/* No wildcards. */
				status = NT_STATUS_OBJECT_PATH_SYNTAX_BAD;
			} else {
				status = hardlink_internals(ctx,
						conn,
						oldname,
						newname);
			}
			break;
		case RENAME_FLAG_COPY:
			if (src_has_wcard || dest_has_wcard) {
				/* No wildcards. */
				status = NT_STATUS_OBJECT_PATH_SYNTAX_BAD;
			} else {
				status = copy_internals(ctx, conn, req, oldname,
							newname, attrs);
			}
			break;
		case RENAME_FLAG_MOVE_CLUSTER_INFORMATION:
			status = NT_STATUS_INVALID_PARAMETER;
			break;
		default:
			status = NT_STATUS_ACCESS_DENIED; /* Default error. */
			break;
	}

	if (!NT_STATUS_IS_OK(status)) {
		if (open_was_deferred(req->mid)) {
			/* We have re-scheduled this call. */
			END_PROFILE(SMBntrename);
			return;
		}

		reply_nterror(req, status);
		END_PROFILE(SMBntrename);
		return;
	}

	reply_outbuf(req, 0, 0);

	END_PROFILE(SMBntrename);
	return;
}

/****************************************************************************
 Reply to a notify change - queue the request and
 don't allow a directory to be opened.
****************************************************************************/

static void call_nt_transact_notify_change(connection_struct *conn,
					   struct smb_request *req,
					   uint16 **ppsetup,
					   uint32 setup_count,
					   char **ppparams,
					   uint32 parameter_count,
					   char **ppdata, uint32 data_count,
					   uint32 max_data_count,
					   uint32 max_param_count)
{
	uint16 *setup = *ppsetup;
	files_struct *fsp;
	uint32 filter;
	NTSTATUS status;
	bool recursive;

	if(setup_count < 6) {
		reply_doserror(req, ERRDOS, ERRbadfunc);
		return;
	}

	fsp = file_fsp(SVAL(setup,4));
	filter = IVAL(setup, 0);
	recursive = (SVAL(setup, 6) != 0) ? True : False;

	DEBUG(3,("call_nt_transact_notify_change\n"));

	if(!fsp) {
		reply_doserror(req, ERRDOS, ERRbadfid);
		return;
	}

	{
		char *filter_string;

		if (!(filter_string = notify_filter_string(NULL, filter))) {
			reply_nterror(req,NT_STATUS_NO_MEMORY);
			return;
		}

		DEBUG(3,("call_nt_transact_notify_change: notify change "
			 "called on %s, filter = %s, recursive = %d\n",
			 fsp->fsp_name, filter_string, recursive));

		TALLOC_FREE(filter_string);
	}

	if((!fsp->is_directory) || (conn != fsp->conn)) {
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return;
	}

	if (fsp->notify == NULL) {

		status = change_notify_create(fsp, filter, recursive);

		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(10, ("change_notify_create returned %s\n",
				   nt_errstr(status)));
			reply_nterror(req, status);
			return;
		}
	}

	if (fsp->notify->num_changes != 0) {

		/*
		 * We've got changes pending, respond immediately
		 */

		/*
		 * TODO: write a torture test to check the filtering behaviour
		 * here.
		 */

		change_notify_reply(req->inbuf, max_param_count, fsp->notify);

		/*
		 * change_notify_reply() above has independently sent its
		 * results
		 */
		return;
	}

	/*
	 * No changes pending, queue the request
	 */

	status = change_notify_add_request(req->inbuf, max_param_count, filter,
			recursive, fsp);
	if (!NT_STATUS_IS_OK(status)) {
		reply_nterror(req, status);
	}
	return;
}

/****************************************************************************
 Reply to an NT transact rename command.
****************************************************************************/

static void call_nt_transact_rename(connection_struct *conn,
				    struct smb_request *req,
				    uint16 **ppsetup, uint32 setup_count,
				    char **ppparams, uint32 parameter_count,
				    char **ppdata, uint32 data_count,
				    uint32 max_data_count)
{
	char *params = *ppparams;
	char *new_name = NULL;
	files_struct *fsp = NULL;
	bool replace_if_exists = False;
	bool dest_has_wcard = False;
	NTSTATUS status;
	TALLOC_CTX *ctx = talloc_tos();

        if(parameter_count < 5) {
		reply_doserror(req, ERRDOS, ERRbadfunc);
		return;
	}

	fsp = file_fsp(SVAL(params, 0));
	replace_if_exists = (SVAL(params,2) & RENAME_REPLACE_IF_EXISTS) ? True : False;
	if (!check_fsp(conn, req, fsp, &current_user)) {
		return;
	}
	srvstr_get_path_wcard(ctx, params, req->flags2, &new_name, params+4,
			      parameter_count - 4,
			      STR_TERMINATE, &status, &dest_has_wcard);
	if (!NT_STATUS_IS_OK(status)) {
		reply_nterror(req, status);
		return;
	}

	status = rename_internals(ctx,
			conn,
			req,
			fsp->fsp_name,
			new_name,
			0,
			replace_if_exists,
			False,
			dest_has_wcard);

	if (!NT_STATUS_IS_OK(status)) {
		if (open_was_deferred(req->mid)) {
			/* We have re-scheduled this call. */
			return;
		}
		reply_nterror(req, status);
		return;
	}

	/*
	 * Rename was successful.
	 */
	send_nt_replies(req, NT_STATUS_OK, NULL, 0, NULL, 0);

	DEBUG(3,("nt transact rename from = %s, to = %s succeeded.\n",
		 fsp->fsp_name, new_name));

	return;
}

/******************************************************************************
 Fake up a completely empty SD.
*******************************************************************************/

static size_t get_null_nt_acl(TALLOC_CTX *mem_ctx, SEC_DESC **ppsd)
{
	size_t sd_size;

	*ppsd = make_standard_sec_desc( mem_ctx, &global_sid_World, &global_sid_World, NULL, &sd_size);
	if(!*ppsd) {
		DEBUG(0,("get_null_nt_acl: Unable to malloc space for security descriptor.\n"));
		sd_size = 0;
	}

	return sd_size;
}

/****************************************************************************
 Reply to query a security descriptor.
****************************************************************************/

static void call_nt_transact_query_security_desc(connection_struct *conn,
						 struct smb_request *req,
						 uint16 **ppsetup,
						 uint32 setup_count,
						 char **ppparams,
						 uint32 parameter_count,
						 char **ppdata,
						 uint32 data_count,
						 uint32 max_data_count)
{
	char *params = *ppparams;
	char *data = *ppdata;
	prs_struct pd;
	SEC_DESC *psd = NULL;
	size_t sd_size;
	uint32 security_info_wanted;
	TALLOC_CTX *mem_ctx;
	files_struct *fsp = NULL;

        if(parameter_count < 8) {
		reply_doserror(req, ERRDOS, ERRbadfunc);
		return;
	}

	fsp = file_fsp(SVAL(params,0));
	if(!fsp) {
		reply_doserror(req, ERRDOS, ERRbadfid);
		return;
	}

	security_info_wanted = IVAL(params,4);

	DEBUG(3,("call_nt_transact_query_security_desc: file = %s, info_wanted = 0x%x\n", fsp->fsp_name,
			(unsigned int)security_info_wanted ));

	params = nttrans_realloc(ppparams, 4);
	if(params == NULL) {
		reply_doserror(req, ERRDOS, ERRnomem);
		return;
	}

	if ((mem_ctx = talloc_init("call_nt_transact_query_security_desc")) == NULL) {
		DEBUG(0,("call_nt_transact_query_security_desc: talloc_init failed.\n"));
		reply_doserror(req, ERRDOS, ERRnomem);
		return;
	}

	/*
	 * Get the permissions to return.
	 */

	if (!lp_nt_acl_support(SNUM(conn))) {
		sd_size = get_null_nt_acl(mem_ctx, &psd);
	} else {
		sd_size = SMB_VFS_FGET_NT_ACL(fsp, fsp->fh->fd, security_info_wanted, &psd);
	}

	if (sd_size == 0) {
		talloc_destroy(mem_ctx);
		reply_unixerror(req, ERRDOS, ERRnoaccess);
		return;
	}

	DEBUG(3,("call_nt_transact_query_security_desc: sd_size = %lu.\n",(unsigned long)sd_size));

	SIVAL(params,0,(uint32)sd_size);

	if(max_data_count < sd_size) {

		send_nt_replies(req, NT_STATUS_BUFFER_TOO_SMALL,
				params, 4, *ppdata, 0);
		talloc_destroy(mem_ctx);
		return;
	}

	/*
	 * Allocate the data we will point this at.
	 */

	data = nttrans_realloc(ppdata, sd_size);
	if(data == NULL) {
		talloc_destroy(mem_ctx);
		reply_doserror(req, ERRDOS, ERRnomem);
		return;
	}

	/*
	 * Init the parse struct we will marshall into.
	 */

	prs_init(&pd, 0, mem_ctx, MARSHALL);

	/*
	 * Setup the prs_struct to point at the memory we just
	 * allocated.
	 */

	prs_give_memory( &pd, data, (uint32)sd_size, False);

	/*
	 * Finally, linearize into the outgoing buffer.
	 */

	if(!sec_io_desc( "sd data", &psd, &pd, 1)) {
		DEBUG(0,("call_nt_transact_query_security_desc: Error in marshalling \
security descriptor.\n"));
		/*
		 * Return access denied for want of a better error message..
		 */
		talloc_destroy(mem_ctx);
		reply_unixerror(req, ERRDOS, ERRnoaccess);
		return;
	}

	/*
	 * Now we can delete the security descriptor.
	 */

	talloc_destroy(mem_ctx);

	send_nt_replies(req, NT_STATUS_OK, params, 4, data, (int)sd_size);
	return;
}

/****************************************************************************
 Reply to set a security descriptor. Map to UNIX perms or POSIX ACLs.
****************************************************************************/

static void call_nt_transact_set_security_desc(connection_struct *conn,
					       struct smb_request *req,
					       uint16 **ppsetup,
					       uint32 setup_count,
					       char **ppparams,
					       uint32 parameter_count,
					       char **ppdata,
					       uint32 data_count,
					       uint32 max_data_count)
{
	char *params= *ppparams;
	char *data = *ppdata;
	files_struct *fsp = NULL;
	uint32 security_info_sent = 0;
	NTSTATUS nt_status;

	if(parameter_count < 8) {
		reply_doserror(req, ERRDOS, ERRbadfunc);
		return;
	}

	if((fsp = file_fsp(SVAL(params,0))) == NULL) {
		reply_doserror(req, ERRDOS, ERRbadfid);
		return;
	}

	if(!lp_nt_acl_support(SNUM(conn))) {
		goto done;
	}

	security_info_sent = IVAL(params,4);

	DEBUG(3,("call_nt_transact_set_security_desc: file = %s, sent 0x%x\n", fsp->fsp_name,
		(unsigned int)security_info_sent ));

	if (data_count == 0) {
		reply_doserror(req, ERRDOS, ERRnoaccess);
		return;
	}

	if (!NT_STATUS_IS_OK(nt_status = set_sd( fsp, data, data_count, security_info_sent))) {
		reply_nterror(req, nt_status);
		return;
	}

  done:

	send_nt_replies(req, NT_STATUS_OK, NULL, 0, NULL, 0);
	return;
}

/****************************************************************************
 Reply to NT IOCTL
****************************************************************************/

static void call_nt_transact_ioctl(connection_struct *conn,
				   struct smb_request *req,
				   uint16 **ppsetup, uint32 setup_count,
				   char **ppparams, uint32 parameter_count,
				   char **ppdata, uint32 data_count,
				   uint32 max_data_count)
{
	uint32 function;
	uint16 fidnum;
	files_struct *fsp;
	uint8 isFSctl;
	uint8 compfilter;
	static bool logged_message;
	char *pdata = *ppdata;

	if (setup_count != 8) {
		DEBUG(3,("call_nt_transact_ioctl: invalid setup count %d\n", setup_count));
		reply_nterror(req, NT_STATUS_NOT_SUPPORTED);
		return;
	}

	function = IVAL(*ppsetup, 0);
	fidnum = SVAL(*ppsetup, 4);
	isFSctl = CVAL(*ppsetup, 6);
	compfilter = CVAL(*ppsetup, 7);

	DEBUG(10,("call_nt_transact_ioctl: function[0x%08X] FID[0x%04X] isFSctl[0x%02X] compfilter[0x%02X]\n", 
		 function, fidnum, isFSctl, compfilter));

	fsp=file_fsp(fidnum);
	/* this check is done in each implemented function case for now
	   because I don't want to break anything... --metze
	FSP_BELONGS_CONN(fsp,conn);*/

	switch (function) {
	case FSCTL_SET_SPARSE:
		/* pretend this succeeded - tho strictly we should
		   mark the file sparse (if the local fs supports it)
		   so we can know if we need to pre-allocate or not */

		DEBUG(10,("FSCTL_SET_SPARSE: called on FID[0x%04X](but not implemented)\n", fidnum));
		send_nt_replies(req, NT_STATUS_OK, NULL, 0, NULL, 0);
		return;

	case FSCTL_CREATE_OR_GET_OBJECT_ID:
	{
		unsigned char objid[16];

		/* This should return the object-id on this file.
		 * I think I'll make this be the inode+dev. JRA.
		 */

		DEBUG(10,("FSCTL_CREATE_OR_GET_OBJECT_ID: called on FID[0x%04X]\n",fidnum));

		if (!fsp_belongs_conn(conn, req, fsp, &current_user)) {
			return;
		}

		data_count = 64;
		pdata = nttrans_realloc(ppdata, data_count);
		if (pdata == NULL) {
			reply_nterror(req, NT_STATUS_NO_MEMORY);
			return;
		}
		push_file_id_16(pdata, &fsp->file_id);
		memcpy(pdata+16,create_volume_objectid(conn,objid),16);
		push_file_id_16(pdata+32, &fsp->file_id);
		send_nt_replies(req, NT_STATUS_OK, NULL, 0,
				pdata, data_count);
		return;
	}

	case FSCTL_GET_REPARSE_POINT:
		/* pretend this fail - my winXP does it like this
		 * --metze
		 */

		DEBUG(10,("FSCTL_GET_REPARSE_POINT: called on FID[0x%04X](but not implemented)\n",fidnum));
		reply_nterror(req, NT_STATUS_NOT_A_REPARSE_POINT);
		return;

	case FSCTL_SET_REPARSE_POINT:
		/* pretend this fail - I'm assuming this because of the FSCTL_GET_REPARSE_POINT case.
		 * --metze
		 */

		DEBUG(10,("FSCTL_SET_REPARSE_POINT: called on FID[0x%04X](but not implemented)\n",fidnum));
		reply_nterror(req, NT_STATUS_NOT_A_REPARSE_POINT);
		return;

	case FSCTL_GET_SHADOW_COPY_DATA: /* don't know if this name is right...*/
	{
		/*
		 * This is called to retrieve the number of Shadow Copies (a.k.a. snapshots)
		 * and return their volume names.  If max_data_count is 16, then it is just
		 * asking for the number of volumes and length of the combined names.
		 *
		 * pdata is the data allocated by our caller, but that uses
		 * total_data_count (which is 0 in our case) rather than max_data_count.
		 * Allocate the correct amount and return the pointer to let
		 * it be deallocated when we return.
		 */
		SHADOW_COPY_DATA *shadow_data = NULL;
		TALLOC_CTX *shadow_mem_ctx = NULL;
		bool labels = False;
		uint32 labels_data_count = 0;
		uint32 i;
		char *cur_pdata;

		if (!fsp_belongs_conn(conn, req, fsp, &current_user)) {
			return;
		}

		if (max_data_count < 16) {
			DEBUG(0,("FSCTL_GET_SHADOW_COPY_DATA: max_data_count(%u) < 16 is invalid!\n",
				max_data_count));
			reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
			return;
		}

		if (max_data_count > 16) {
			labels = True;
		}

		shadow_mem_ctx = talloc_init("SHADOW_COPY_DATA");
		if (shadow_mem_ctx == NULL) {
			DEBUG(0,("talloc_init(SHADOW_COPY_DATA) failed!\n"));
			reply_nterror(req, NT_STATUS_NO_MEMORY);
			return;
		}

		shadow_data = TALLOC_ZERO_P(shadow_mem_ctx,SHADOW_COPY_DATA);
		if (shadow_data == NULL) {
			DEBUG(0,("TALLOC_ZERO() failed!\n"));
			talloc_destroy(shadow_mem_ctx);
			reply_nterror(req, NT_STATUS_NO_MEMORY);
			return;
		}

		shadow_data->mem_ctx = shadow_mem_ctx;

		/*
		 * Call the VFS routine to actually do the work.
		 */
		if (SMB_VFS_GET_SHADOW_COPY_DATA(fsp, shadow_data, labels)!=0) {
			talloc_destroy(shadow_data->mem_ctx);
			if (errno == ENOSYS) {
				DEBUG(5,("FSCTL_GET_SHADOW_COPY_DATA: connectpath %s, not supported.\n", 
					conn->connectpath));
				reply_nterror(req, NT_STATUS_NOT_SUPPORTED);
				return;
			} else {
				DEBUG(0,("FSCTL_GET_SHADOW_COPY_DATA: connectpath %s, failed.\n", 
					conn->connectpath));
				reply_nterror(req, NT_STATUS_UNSUCCESSFUL);
				return;
			}
		}

		labels_data_count = (shadow_data->num_volumes*2*sizeof(SHADOW_COPY_LABEL))+2;

		if (!labels) {
			data_count = 16;
		} else {
			data_count = 12+labels_data_count+4;
		}

		if (max_data_count<data_count) {
			DEBUG(0,("FSCTL_GET_SHADOW_COPY_DATA: max_data_count(%u) too small (%u) bytes needed!\n",
				max_data_count,data_count));
			talloc_destroy(shadow_data->mem_ctx);
			reply_nterror(req, NT_STATUS_BUFFER_TOO_SMALL);
			return;
		}

		pdata = nttrans_realloc(ppdata, data_count);
		if (pdata == NULL) {
			talloc_destroy(shadow_data->mem_ctx);
			reply_nterror(req, NT_STATUS_NO_MEMORY);
			return;
		}

		cur_pdata = pdata;

		/* num_volumes 4 bytes */
		SIVAL(pdata,0,shadow_data->num_volumes);

		if (labels) {
			/* num_labels 4 bytes */
			SIVAL(pdata,4,shadow_data->num_volumes);
		}

		/* needed_data_count 4 bytes */
		SIVAL(pdata,8,labels_data_count);

		cur_pdata+=12;

		DEBUG(10,("FSCTL_GET_SHADOW_COPY_DATA: %u volumes for path[%s].\n",
			shadow_data->num_volumes,fsp->fsp_name));
		if (labels && shadow_data->labels) {
			for (i=0;i<shadow_data->num_volumes;i++) {
				srvstr_push(pdata, req->flags2,
					    cur_pdata, shadow_data->labels[i],
					    2*sizeof(SHADOW_COPY_LABEL),
					    STR_UNICODE|STR_TERMINATE);
				cur_pdata+=2*sizeof(SHADOW_COPY_LABEL);
				DEBUGADD(10,("Label[%u]: '%s'\n",i,shadow_data->labels[i]));
			}
		}

		talloc_destroy(shadow_data->mem_ctx);

		send_nt_replies(req, NT_STATUS_OK, NULL, 0,
				pdata, data_count);

		return;
        }

	case FSCTL_FIND_FILES_BY_SID: /* I hope this name is right */
	{
		/* pretend this succeeded -
		 *
		 * we have to send back a list with all files owned by this SID
		 *
		 * but I have to check that --metze
		 */
		DOM_SID sid;
		uid_t uid;
		size_t sid_len = MIN(data_count-4,SID_MAX_SIZE);

		DEBUG(10,("FSCTL_FIND_FILES_BY_SID: called on FID[0x%04X]\n",fidnum));

		if (!fsp_belongs_conn(conn, req, fsp, &current_user)) {
			return;
		}

		/* unknown 4 bytes: this is not the length of the sid :-(  */
		/*unknown = IVAL(pdata,0);*/

		sid_parse(pdata+4,sid_len,&sid);
		DEBUGADD(10,("for SID: %s\n",sid_string_static(&sid)));

		if (!sid_to_uid(&sid, &uid)) {
			DEBUG(0,("sid_to_uid: failed, sid[%s] sid_len[%lu]\n",
				sid_string_static(&sid),(unsigned long)sid_len));
			uid = (-1);
		}

		/* we can take a look at the find source :-)
		 *
		 * find ./ -uid $uid  -name '*'   is what we need here
		 *
		 *
		 * and send 4bytes len and then NULL terminated unicode strings
		 * for each file
		 *
		 * but I don't know how to deal with the paged results
		 * (maybe we can hang the result anywhere in the fsp struct)
		 *
		 * we don't send all files at once
		 * and at the next we should *not* start from the beginning,
		 * so we have to cache the result
		 *
		 * --metze
		 */

		/* this works for now... */
		send_nt_replies(req, NT_STATUS_OK, NULL, 0, NULL, 0);
		return;
	}
	default:
		if (!logged_message) {
			logged_message = True; /* Only print this once... */
			DEBUG(0,("call_nt_transact_ioctl(0x%x): Currently not implemented.\n",
				 function));
		}
	}

	reply_nterror(req, NT_STATUS_NOT_SUPPORTED);
}


#ifdef HAVE_SYS_QUOTAS
/****************************************************************************
 Reply to get user quota
****************************************************************************/

static void call_nt_transact_get_user_quota(connection_struct *conn,
					    struct smb_request *req,
					    uint16 **ppsetup,
					    uint32 setup_count,
					    char **ppparams,
					    uint32 parameter_count,
					    char **ppdata,
					    uint32 data_count,
					    uint32 max_data_count)
{
	NTSTATUS nt_status = NT_STATUS_OK;
	char *params = *ppparams;
	char *pdata = *ppdata;
	char *entry;
	int data_len=0,param_len=0;
	int qt_len=0;
	int entry_len = 0;
	files_struct *fsp = NULL;
	uint16 level = 0;
	size_t sid_len;
	DOM_SID sid;
	bool start_enum = True;
	SMB_NTQUOTA_STRUCT qt;
	SMB_NTQUOTA_LIST *tmp_list;
	SMB_NTQUOTA_HANDLE *qt_handle = NULL;

	ZERO_STRUCT(qt);

	/* access check */
	if (current_user.ut.uid != 0) {
		DEBUG(1,("get_user_quota: access_denied service [%s] user [%s]\n",
			lp_servicename(SNUM(conn)),conn->user));
		reply_doserror(req, ERRDOS, ERRnoaccess);
		return;
	}

	/*
	 * Ensure minimum number of parameters sent.
	 */

	if (parameter_count < 4) {
		DEBUG(0,("TRANSACT_GET_USER_QUOTA: requires %d >= 4 bytes parameters\n",parameter_count));
		reply_doserror(req, ERRDOS, ERRinvalidparam);
		return;
	}

	/* maybe we can check the quota_fnum */
	fsp = file_fsp(SVAL(params,0));
	if (!CHECK_NTQUOTA_HANDLE_OK(fsp,conn)) {
		DEBUG(3,("TRANSACT_GET_USER_QUOTA: no valid QUOTA HANDLE\n"));
		reply_nterror(req, NT_STATUS_INVALID_HANDLE);
		return;
	}

	/* the NULL pointer checking for fsp->fake_file_handle->pd
	 * is done by CHECK_NTQUOTA_HANDLE_OK()
	 */
	qt_handle = (SMB_NTQUOTA_HANDLE *)fsp->fake_file_handle->pd;

	level = SVAL(params,2);

	/* unknown 12 bytes leading in params */

	switch (level) {
		case TRANSACT_GET_USER_QUOTA_LIST_CONTINUE:
			/* seems that we should continue with the enum here --metze */

			if (qt_handle->quota_list!=NULL &&
			    qt_handle->tmp_list==NULL) {

				/* free the list */
				free_ntquota_list(&(qt_handle->quota_list));

				/* Realloc the size of parameters and data we will return */
				param_len = 4;
				params = nttrans_realloc(ppparams, param_len);
				if(params == NULL) {
					reply_doserror(req, ERRDOS, ERRnomem);
					return;
				}

				data_len = 0;
				SIVAL(params,0,data_len);

				break;
			}

			start_enum = False;

		case TRANSACT_GET_USER_QUOTA_LIST_START:

			if (qt_handle->quota_list==NULL &&
				qt_handle->tmp_list==NULL) {
				start_enum = True;
			}

			if (start_enum && vfs_get_user_ntquota_list(fsp,&(qt_handle->quota_list))!=0) {
				reply_doserror(req, ERRSRV, ERRerror);
				return;
			}

			/* Realloc the size of parameters and data we will return */
			param_len = 4;
			params = nttrans_realloc(ppparams, param_len);
			if(params == NULL) {
				reply_doserror(req, ERRDOS, ERRnomem);
				return;
			}

			/* we should not trust the value in max_data_count*/
			max_data_count = MIN(max_data_count,2048);

			pdata = nttrans_realloc(ppdata, max_data_count);/* should be max data count from client*/
			if(pdata == NULL) {
				reply_doserror(req, ERRDOS, ERRnomem);
				return;
			}

			entry = pdata;

			/* set params Size of returned Quota Data 4 bytes*/
			/* but set it later when we know it */

			/* for each entry push the data */

			if (start_enum) {
				qt_handle->tmp_list = qt_handle->quota_list;
			}

			tmp_list = qt_handle->tmp_list;

			for (;((tmp_list!=NULL)&&((qt_len +40+SID_MAX_SIZE)<max_data_count));
				tmp_list=tmp_list->next,entry+=entry_len,qt_len+=entry_len) {

				sid_len = sid_size(&tmp_list->quotas->sid);
				entry_len = 40 + sid_len;

				/* nextoffset entry 4 bytes */
				SIVAL(entry,0,entry_len);

				/* then the len of the SID 4 bytes */
				SIVAL(entry,4,sid_len);

				/* unknown data 8 bytes SMB_BIG_UINT */
				SBIG_UINT(entry,8,(SMB_BIG_UINT)0); /* this is not 0 in windows...-metze*/

				/* the used disk space 8 bytes SMB_BIG_UINT */
				SBIG_UINT(entry,16,tmp_list->quotas->usedspace);

				/* the soft quotas 8 bytes SMB_BIG_UINT */
				SBIG_UINT(entry,24,tmp_list->quotas->softlim);

				/* the hard quotas 8 bytes SMB_BIG_UINT */
				SBIG_UINT(entry,32,tmp_list->quotas->hardlim);

				/* and now the SID */
				sid_linearize(entry+40, sid_len, &tmp_list->quotas->sid);
			}

			qt_handle->tmp_list = tmp_list;

			/* overwrite the offset of the last entry */
			SIVAL(entry-entry_len,0,0);

			data_len = 4+qt_len;
			/* overwrite the params quota_data_len */
			SIVAL(params,0,data_len);

			break;

		case TRANSACT_GET_USER_QUOTA_FOR_SID:

			/* unknown 4 bytes IVAL(pdata,0) */

			if (data_count < 8) {
				DEBUG(0,("TRANSACT_GET_USER_QUOTA_FOR_SID: requires %d >= %d bytes data\n",data_count,8));
				reply_doserror(req, ERRDOS, ERRunknownlevel);
				return;
			}

			sid_len = IVAL(pdata,4);
			/* Ensure this is less than 1mb. */
			if (sid_len > (1024*1024)) {
				reply_doserror(req, ERRDOS, ERRnomem);
				return;
			}

			if (data_count < 8+sid_len) {
				DEBUG(0,("TRANSACT_GET_USER_QUOTA_FOR_SID: requires %d >= %lu bytes data\n",data_count,(unsigned long)(8+sid_len)));
				reply_doserror(req, ERRDOS, ERRunknownlevel);
				return;
			}

			data_len = 4+40+sid_len;

			if (max_data_count < data_len) {
				DEBUG(0,("TRANSACT_GET_USER_QUOTA_FOR_SID: max_data_count(%d) < data_len(%d)\n",
					max_data_count, data_len));
				param_len = 4;
				SIVAL(params,0,data_len);
				data_len = 0;
				nt_status = NT_STATUS_BUFFER_TOO_SMALL;
				break;
			}

			sid_parse(pdata+8,sid_len,&sid);

			if (vfs_get_ntquota(fsp, SMB_USER_QUOTA_TYPE, &sid, &qt)!=0) {
				ZERO_STRUCT(qt);
				/*
				 * we have to return zero's in all fields
				 * instead of returning an error here
				 * --metze
				 */
			}

			/* Realloc the size of parameters and data we will return */
			param_len = 4;
			params = nttrans_realloc(ppparams, param_len);
			if(params == NULL) {
				reply_doserror(req, ERRDOS, ERRnomem);
				return;
			}

			pdata = nttrans_realloc(ppdata, data_len);
			if(pdata == NULL) {
				reply_doserror(req, ERRDOS, ERRnomem);
				return;
			}

			entry = pdata;

			/* set params Size of returned Quota Data 4 bytes*/
			SIVAL(params,0,data_len);

			/* nextoffset entry 4 bytes */
			SIVAL(entry,0,0);

			/* then the len of the SID 4 bytes */
			SIVAL(entry,4,sid_len);

			/* unknown data 8 bytes SMB_BIG_UINT */
			SBIG_UINT(entry,8,(SMB_BIG_UINT)0); /* this is not 0 in windows...-mezte*/

			/* the used disk space 8 bytes SMB_BIG_UINT */
			SBIG_UINT(entry,16,qt.usedspace);

			/* the soft quotas 8 bytes SMB_BIG_UINT */
			SBIG_UINT(entry,24,qt.softlim);

			/* the hard quotas 8 bytes SMB_BIG_UINT */
			SBIG_UINT(entry,32,qt.hardlim);

			/* and now the SID */
			sid_linearize(entry+40, sid_len, &sid);

			break;

		default:
			DEBUG(0,("do_nt_transact_get_user_quota: fnum %d unknown level 0x%04hX\n",fsp->fnum,level));
			reply_doserror(req, ERRSRV, ERRerror);
			return;
			break;
	}

	send_nt_replies(req, nt_status, params, param_len,
			pdata, data_len);
}

/****************************************************************************
 Reply to set user quota
****************************************************************************/

static void call_nt_transact_set_user_quota(connection_struct *conn,
					    struct smb_request *req,
					    uint16 **ppsetup,
					    uint32 setup_count,
					    char **ppparams,
					    uint32 parameter_count,
					    char **ppdata,
					    uint32 data_count,
					    uint32 max_data_count)
{
	char *params = *ppparams;
	char *pdata = *ppdata;
	int data_len=0,param_len=0;
	SMB_NTQUOTA_STRUCT qt;
	size_t sid_len;
	DOM_SID sid;
	files_struct *fsp = NULL;

	ZERO_STRUCT(qt);

	/* access check */
	if (current_user.ut.uid != 0) {
		DEBUG(1,("set_user_quota: access_denied service [%s] user [%s]\n",
			lp_servicename(SNUM(conn)),conn->user));
		reply_doserror(req, ERRDOS, ERRnoaccess);
		return;
	}

	/*
	 * Ensure minimum number of parameters sent.
	 */

	if (parameter_count < 2) {
		DEBUG(0,("TRANSACT_SET_USER_QUOTA: requires %d >= 2 bytes parameters\n",parameter_count));
		reply_doserror(req, ERRDOS, ERRinvalidparam);
		return;
	}

	/* maybe we can check the quota_fnum */
	fsp = file_fsp(SVAL(params,0));
	if (!CHECK_NTQUOTA_HANDLE_OK(fsp,conn)) {
		DEBUG(3,("TRANSACT_GET_USER_QUOTA: no valid QUOTA HANDLE\n"));
		reply_nterror(req, NT_STATUS_INVALID_HANDLE);
		return;
	}

	if (data_count < 40) {
		DEBUG(0,("TRANSACT_SET_USER_QUOTA: requires %d >= %d bytes data\n",data_count,40));
		reply_doserror(req, ERRDOS, ERRunknownlevel);
		return;
	}

	/* offset to next quota record.
	 * 4 bytes IVAL(pdata,0)
	 * unused here...
	 */

	/* sid len */
	sid_len = IVAL(pdata,4);

	if (data_count < 40+sid_len) {
		DEBUG(0,("TRANSACT_SET_USER_QUOTA: requires %d >= %lu bytes data\n",data_count,(unsigned long)40+sid_len));
		reply_doserror(req, ERRDOS, ERRunknownlevel);
		return;
	}

	/* unknown 8 bytes in pdata
	 * maybe its the change time in NTTIME
	 */

	/* the used space 8 bytes (SMB_BIG_UINT)*/
	qt.usedspace = (SMB_BIG_UINT)IVAL(pdata,16);
#ifdef LARGE_SMB_OFF_T
	qt.usedspace |= (((SMB_BIG_UINT)IVAL(pdata,20)) << 32);
#else /* LARGE_SMB_OFF_T */
	if ((IVAL(pdata,20) != 0)&&
		((qt.usedspace != 0xFFFFFFFF)||
		(IVAL(pdata,20)!=0xFFFFFFFF))) {
		/* more than 32 bits? */
		reply_doserror(req, ERRDOS, ERRunknownlevel);
		return;
	}
#endif /* LARGE_SMB_OFF_T */

	/* the soft quotas 8 bytes (SMB_BIG_UINT)*/
	qt.softlim = (SMB_BIG_UINT)IVAL(pdata,24);
#ifdef LARGE_SMB_OFF_T
	qt.softlim |= (((SMB_BIG_UINT)IVAL(pdata,28)) << 32);
#else /* LARGE_SMB_OFF_T */
	if ((IVAL(pdata,28) != 0)&&
		((qt.softlim != 0xFFFFFFFF)||
		(IVAL(pdata,28)!=0xFFFFFFFF))) {
		/* more than 32 bits? */
		reply_doserror(req, ERRDOS, ERRunknownlevel);
		return;
	}
#endif /* LARGE_SMB_OFF_T */

	/* the hard quotas 8 bytes (SMB_BIG_UINT)*/
	qt.hardlim = (SMB_BIG_UINT)IVAL(pdata,32);
#ifdef LARGE_SMB_OFF_T
	qt.hardlim |= (((SMB_BIG_UINT)IVAL(pdata,36)) << 32);
#else /* LARGE_SMB_OFF_T */
	if ((IVAL(pdata,36) != 0)&&
		((qt.hardlim != 0xFFFFFFFF)||
		(IVAL(pdata,36)!=0xFFFFFFFF))) {
		/* more than 32 bits? */
		reply_doserror(req, ERRDOS, ERRunknownlevel);
		return;
	}
#endif /* LARGE_SMB_OFF_T */

	sid_parse(pdata+40,sid_len,&sid);
	DEBUGADD(8,("SID: %s\n",sid_string_static(&sid)));

	/* 44 unknown bytes left... */

	if (vfs_set_ntquota(fsp, SMB_USER_QUOTA_TYPE, &sid, &qt)!=0) {
		reply_doserror(req, ERRSRV, ERRerror);
		return;
	}

	send_nt_replies(req, NT_STATUS_OK, params, param_len,
			pdata, data_len);
}
#endif /* HAVE_SYS_QUOTAS */

static void handle_nttrans(connection_struct *conn,
			   struct trans_state *state,
			   struct smb_request *req)
{
	if (Protocol >= PROTOCOL_NT1) {
		req->flags2 |= 0x40; /* IS_LONG_NAME */
		SSVAL(req->inbuf,smb_flg2,req->flags2);
	}

	/* Now we must call the relevant NT_TRANS function */
	switch(state->call) {
		case NT_TRANSACT_CREATE:
		{
			START_PROFILE(NT_transact_create);
			call_nt_transact_create(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return);
			END_PROFILE(NT_transact_create);
			break;
		}

		case NT_TRANSACT_IOCTL:
		{
			START_PROFILE(NT_transact_ioctl);
			call_nt_transact_ioctl(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return);
			END_PROFILE(NT_transact_ioctl);
			break;
		}

		case NT_TRANSACT_SET_SECURITY_DESC:
		{
			START_PROFILE(NT_transact_set_security_desc);
			call_nt_transact_set_security_desc(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return);
			END_PROFILE(NT_transact_set_security_desc);
			break;
		}

		case NT_TRANSACT_NOTIFY_CHANGE:
		{
			START_PROFILE(NT_transact_notify_change);
			call_nt_transact_notify_change(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return,
				state->max_param_return);
			END_PROFILE(NT_transact_notify_change);
			break;
		}

		case NT_TRANSACT_RENAME:
		{
			START_PROFILE(NT_transact_rename);
			call_nt_transact_rename(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return);
			END_PROFILE(NT_transact_rename);
			break;
		}

		case NT_TRANSACT_QUERY_SECURITY_DESC:
		{
			START_PROFILE(NT_transact_query_security_desc);
			call_nt_transact_query_security_desc(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return);
			END_PROFILE(NT_transact_query_security_desc);
			break;
		}

#ifdef HAVE_SYS_QUOTAS
		case NT_TRANSACT_GET_USER_QUOTA:
		{
			START_PROFILE(NT_transact_get_user_quota);
			call_nt_transact_get_user_quota(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return);
			END_PROFILE(NT_transact_get_user_quota);
			break;
		}

		case NT_TRANSACT_SET_USER_QUOTA:
		{
			START_PROFILE(NT_transact_set_user_quota);
			call_nt_transact_set_user_quota(
				conn, req,
				&state->setup, state->setup_count,
				&state->param, state->total_param,
				&state->data, state->total_data,
				state->max_data_return);
			END_PROFILE(NT_transact_set_user_quota);
			break;
		}
#endif /* HAVE_SYS_QUOTAS */

		default:
			/* Error in request */
			DEBUG(0,("handle_nttrans: Unknown request %d in "
				 "nttrans call\n", state->call));
			reply_doserror(req, ERRSRV, ERRerror);
			return;
	}
	return;
}

/****************************************************************************
 Reply to a SMBNTtrans.
****************************************************************************/

void reply_nttrans(connection_struct *conn, struct smb_request *req)
{
	uint32 pscnt;
	uint32 psoff;
	uint32 dscnt;
	uint32 dsoff;
	uint16 function_code;
	NTSTATUS result;
	struct trans_state *state;
	int size;

	START_PROFILE(SMBnttrans);

	if (req->wct < 19) {
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		END_PROFILE(SMBnttrans);
		return;
	}

	size = smb_len(req->inbuf) + 4;
	pscnt = IVAL(req->inbuf,smb_nt_ParameterCount);
	psoff = IVAL(req->inbuf,smb_nt_ParameterOffset);
	dscnt = IVAL(req->inbuf,smb_nt_DataCount);
	dsoff = IVAL(req->inbuf,smb_nt_DataOffset);
	function_code = SVAL(req->inbuf, smb_nt_Function);

	if (IS_IPC(conn) && (function_code != NT_TRANSACT_CREATE)) {
		reply_doserror(req, ERRSRV, ERRaccess);
		END_PROFILE(SMBnttrans);
		return;
	}

	result = allow_new_trans(conn->pending_trans, req->mid);
	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(2, ("Got invalid nttrans request: %s\n", nt_errstr(result)));
		reply_nterror(req, result);
		END_PROFILE(SMBnttrans);
		return;
	}

	if ((state = TALLOC_P(conn->mem_ctx, struct trans_state)) == NULL) {
		reply_doserror(req, ERRSRV, ERRaccess);
		END_PROFILE(SMBnttrans);
		return;
	}

	state->cmd = SMBnttrans;

	state->mid = req->mid;
	state->vuid = req->vuid;
	state->total_data = IVAL(req->inbuf, smb_nt_TotalDataCount);
	state->data = NULL;
	state->total_param = IVAL(req->inbuf, smb_nt_TotalParameterCount);
	state->param = NULL;
	state->max_data_return = IVAL(req->inbuf,smb_nt_MaxDataCount);
	state->max_param_return = IVAL(req->inbuf,smb_nt_MaxParameterCount);

	/* setup count is in *words* */
	state->setup_count = 2*CVAL(req->inbuf,smb_nt_SetupCount);
	state->setup = NULL;
	state->call = function_code;

	/*
	 * All nttrans messages we handle have smb_wct == 19 +
	 * state->setup_count.  Ensure this is so as a sanity check.
	 */

	if(req->wct != 19 + (state->setup_count/2)) {
		DEBUG(2,("Invalid smb_wct %d in nttrans call (should be %d)\n",
			 req->wct, 19 + (state->setup_count/2)));
		goto bad_param;
	}

	/* Don't allow more than 128mb for each value. */
	if ((state->total_data > (1024*1024*128)) ||
	    (state->total_param > (1024*1024*128))) {
		reply_doserror(req, ERRDOS, ERRnomem);
		END_PROFILE(SMBnttrans);
		return;
	}

	if ((dscnt > state->total_data) || (pscnt > state->total_param))
		goto bad_param;

	if (state->total_data)  {
		/* Can't use talloc here, the core routines do realloc on the
		 * params and data. */
		if ((state->data = (char *)SMB_MALLOC(state->total_data)) == NULL) {
			DEBUG(0,("reply_nttrans: data malloc fail for %u "
				 "bytes !\n", (unsigned int)state->total_data));
			TALLOC_FREE(state);
			reply_doserror(req, ERRDOS, ERRnomem);
			END_PROFILE(SMBnttrans);
			return;
		}
		if ((dsoff+dscnt < dsoff) || (dsoff+dscnt < dscnt))
			goto bad_param;
		if ((smb_base(req->inbuf)+dsoff+dscnt
		     > (char *)req->inbuf + size) ||
		    (smb_base(req->inbuf)+dsoff+dscnt < smb_base(req->inbuf)))
			goto bad_param;

		memcpy(state->data,smb_base(req->inbuf)+dsoff,dscnt);
	}

	if (state->total_param) {
		/* Can't use talloc here, the core routines do realloc on the
		 * params and data. */
		if ((state->param = (char *)SMB_MALLOC(state->total_param)) == NULL) {
			DEBUG(0,("reply_nttrans: param malloc fail for %u "
				 "bytes !\n", (unsigned int)state->total_param));
			SAFE_FREE(state->data);
			TALLOC_FREE(state);
			reply_doserror(req, ERRDOS, ERRnomem);
			END_PROFILE(SMBnttrans);
			return;
		}
		if ((psoff+pscnt < psoff) || (psoff+pscnt < pscnt))
			goto bad_param;
		if ((smb_base(req->inbuf)+psoff+pscnt
		     > (char *)req->inbuf + size) ||
		    (smb_base(req->inbuf)+psoff+pscnt < smb_base(req->inbuf)))
			goto bad_param;

		memcpy(state->param,smb_base(req->inbuf)+psoff,pscnt);
	}

	state->received_data  = dscnt;
	state->received_param = pscnt;

	if(state->setup_count > 0) {
		DEBUG(10,("reply_nttrans: state->setup_count = %d\n",
			  state->setup_count));
		state->setup = (uint16 *)TALLOC(state, state->setup_count);
		if (state->setup == NULL) {
			DEBUG(0,("reply_nttrans : Out of memory\n"));
			SAFE_FREE(state->data);
			SAFE_FREE(state->param);
			TALLOC_FREE(state);
			reply_doserror(req, ERRDOS, ERRnomem);
			END_PROFILE(SMBnttrans);
			return;
		}

		if ((smb_nt_SetupStart + state->setup_count < smb_nt_SetupStart) ||
		    (smb_nt_SetupStart + state->setup_count < state->setup_count)) {
			goto bad_param;
		}
		if (smb_nt_SetupStart + state->setup_count > size) {
			goto bad_param;
		}

		memcpy( state->setup, &req->inbuf[smb_nt_SetupStart],
			state->setup_count);
		dump_data(10, (uint8 *)state->setup, state->setup_count);
	}

	if ((state->received_data == state->total_data) &&
	    (state->received_param == state->total_param)) {
		handle_nttrans(conn, state, req);
		SAFE_FREE(state->param);
		SAFE_FREE(state->data);
		TALLOC_FREE(state);
		END_PROFILE(SMBnttrans);
		return;
	}

	DLIST_ADD(conn->pending_trans, state);

	/* We need to send an interim response then receive the rest
	   of the parameter/data bytes */
	reply_outbuf(req, 0, 0);
	show_msg((char *)req->outbuf);
	END_PROFILE(SMBnttrans);
	return;

  bad_param:

	DEBUG(0,("reply_nttrans: invalid trans parameters\n"));
	SAFE_FREE(state->data);
	SAFE_FREE(state->param);
	TALLOC_FREE(state);
	reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
	END_PROFILE(SMBnttrans);
	return;
}

/****************************************************************************
 Reply to a SMBnttranss
 ****************************************************************************/

void reply_nttranss(connection_struct *conn, struct smb_request *req)
{
	unsigned int pcnt,poff,dcnt,doff,pdisp,ddisp;
	struct trans_state *state;

	int size;

	START_PROFILE(SMBnttranss);

	show_msg((char *)req->inbuf);

	if (req->wct < 18) {
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		END_PROFILE(SMBnttranss);
		return;
	}

	for (state = conn->pending_trans; state != NULL;
	     state = state->next) {
		if (state->mid == req->mid) {
			break;
		}
	}

	if ((state == NULL) || (state->cmd != SMBnttrans)) {
		reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
		END_PROFILE(SMBnttranss);
		return;
	}

	/* Revise state->total_param and state->total_data in case they have
	   changed downwards */
	if (IVAL(req->inbuf, smb_nts_TotalParameterCount)
	    < state->total_param) {
		state->total_param = IVAL(req->inbuf,
					  smb_nts_TotalParameterCount);
	}
	if (IVAL(req->inbuf, smb_nts_TotalDataCount) < state->total_data) {
		state->total_data = IVAL(req->inbuf, smb_nts_TotalDataCount);
	}

	size = smb_len(req->inbuf) + 4;

	pcnt = IVAL(req->inbuf,smb_nts_ParameterCount);
	poff = IVAL(req->inbuf, smb_nts_ParameterOffset);
	pdisp = IVAL(req->inbuf, smb_nts_ParameterDisplacement);

	dcnt = IVAL(req->inbuf, smb_nts_DataCount);
	ddisp = IVAL(req->inbuf, smb_nts_DataDisplacement);
	doff = IVAL(req->inbuf, smb_nts_DataOffset);

	state->received_param += pcnt;
	state->received_data += dcnt;

	if ((state->received_data > state->total_data) ||
	    (state->received_param > state->total_param))
		goto bad_param;

	if (pcnt) {
		if (pdisp+pcnt > state->total_param)
			goto bad_param;
		if ((pdisp+pcnt < pdisp) || (pdisp+pcnt < pcnt))
			goto bad_param;
		if (pdisp > state->total_param)
			goto bad_param;
		if ((smb_base(req->inbuf) + poff + pcnt
		     > (char *)req->inbuf + size) ||
		    (smb_base(req->inbuf) + poff + pcnt
		     < smb_base(req->inbuf)))
			goto bad_param;
		if (state->param + pdisp < state->param)
			goto bad_param;

		memcpy(state->param+pdisp, smb_base(req->inbuf)+poff,
		       pcnt);
	}

	if (dcnt) {
		if (ddisp+dcnt > state->total_data)
			goto bad_param;
		if ((ddisp+dcnt < ddisp) || (ddisp+dcnt < dcnt))
			goto bad_param;
		if (ddisp > state->total_data)
			goto bad_param;
		if ((smb_base(req->inbuf) + doff + dcnt
		     > (char *)req->inbuf + size) ||
		    (smb_base(req->inbuf) + doff + dcnt
		     < smb_base(req->inbuf)))
			goto bad_param;
		if (state->data + ddisp < state->data)
			goto bad_param;

		memcpy(state->data+ddisp, smb_base(req->inbuf)+doff,
		       dcnt);
	}

	if ((state->received_param < state->total_param) ||
	    (state->received_data < state->total_data)) {
		END_PROFILE(SMBnttranss);
		return;
	}

	/*
	 * construct_reply_common will copy smb_com from inbuf to
	 * outbuf. SMBnttranss is wrong here.
	 */
	SCVAL(req->inbuf,smb_com,SMBnttrans);

	handle_nttrans(conn, state, req);

	DLIST_REMOVE(conn->pending_trans, state);
	SAFE_FREE(state->data);
	SAFE_FREE(state->param);
	TALLOC_FREE(state);
	END_PROFILE(SMBnttranss);
	return;

  bad_param:

	DEBUG(0,("reply_nttranss: invalid trans parameters\n"));
	DLIST_REMOVE(conn->pending_trans, state);
	SAFE_FREE(state->data);
	SAFE_FREE(state->param);
	TALLOC_FREE(state);
	reply_nterror(req, NT_STATUS_INVALID_PARAMETER);
	END_PROFILE(SMBnttranss);
	return;
}
