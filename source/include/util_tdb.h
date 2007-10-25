/* 
   Unix SMB/CIFS implementation.
   tdb utility functions
   Copyright (C) Andrew Tridgell 1999
   
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

#ifndef __TDBUTIL_H__
#define __TDBUTIL_H__

#include "tdb.h"

/* single node of a list returned by tdb_search_keys */
typedef struct keys_node 
{
	struct keys_node *prev, *next;
	TDB_DATA node_key;
} TDB_LIST_NODE;

struct tdb_wrap {
	struct tdb_context *tdb;
	const char *name;
	struct tdb_wrap *next, *prev;
};

struct tdb_validation_status {
	bool tdb_error;
	bool bad_freelist;
	bool bad_entry;
	bool unknown_key;
	bool success;
};

typedef int (*tdb_validate_data_func)(TDB_CONTEXT *the_tdb, TDB_DATA kbuf, TDB_DATA dbuf, void *state);

TDB_LIST_NODE *tdb_search_keys(struct tdb_context*, const char*);
void tdb_search_list_free(TDB_LIST_NODE*);
int32 tdb_change_int32_atomic(struct tdb_context *tdb, const char *keystr, int32 *oldval, int32 change_val);
int tdb_lock_bystring(struct tdb_context *tdb, const char *keyval);
int tdb_lock_bystring_with_timeout(TDB_CONTEXT *tdb, const char *keyval,
				   int timeout);
void tdb_unlock_bystring(struct tdb_context *tdb, const char *keyval);
int tdb_read_lock_bystring_with_timeout(TDB_CONTEXT *tdb, const char *keyval,
					unsigned int timeout);
void tdb_read_unlock_bystring(TDB_CONTEXT *tdb, const char *keyval);
int32 tdb_fetch_int32(struct tdb_context *tdb, const char *keystr);
bool tdb_store_uint32(struct tdb_context *tdb, const char *keystr, uint32 value);
int tdb_store_int32(struct tdb_context *tdb, const char *keystr, int32 v);
bool tdb_fetch_uint32(struct tdb_context *tdb, const char *keystr, uint32 *value);
int tdb_traverse_delete_fn(struct tdb_context *the_tdb, TDB_DATA key, TDB_DATA dbuf,
                     void *state);
int tdb_store_bystring(struct tdb_context *tdb, const char *keystr, TDB_DATA data, int flags);
TDB_DATA tdb_fetch_bystring(struct tdb_context *tdb, const char *keystr);
int tdb_delete_bystring(struct tdb_context *tdb, const char *keystr);
struct tdb_context *tdb_open_log(const char *name, int hash_size,
				 int tdb_flags, int open_flags, mode_t mode);
int tdb_unpack(const uint8 *buf, int bufsize, const char *fmt, ...);
size_t tdb_pack(uint8 *buf, int bufsize, const char *fmt, ...);
TDB_DATA make_tdb_data(const uint8 *dptr, size_t dsize);
TDB_DATA string_tdb_data(const char *string);
TDB_DATA string_term_tdb_data(const char *string);
int tdb_trans_store(struct tdb_context *tdb, TDB_DATA key, TDB_DATA dbuf,
		    int flag);
bool tdb_change_uint32_atomic(TDB_CONTEXT *tdb, const char *keystr,
			      uint32 *oldval, uint32 change_val);
int tdb_chainlock_with_timeout( TDB_CONTEXT *tdb, TDB_DATA key,
				unsigned int timeout);

#endif /* __TDBUTIL_H__ */
