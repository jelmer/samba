/* 
 *  Unix SMB/CIFS implementation.
 *  Virtual Windows Registry Layer
 *  Copyright (C) Gerald Carter                     2002-2005
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/* Implementation of internal registry database functions. */

#include "includes.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_REGISTRY

static struct tdb_wrap *tdb_reg = NULL;
static int tdb_refcount;

/* List the deepest path into the registry.  All part components will be created.*/

/* If you want to have a part of the path controlled by the tdb and part by
   a virtual registry db (e.g. printing), then you have to list the deepest path.
   For example,"HKLM/SOFTWARE/Microsoft/Windows NT/CurrentVersion/Print" 
   allows the reg_db backend to handle everything up to 
   "HKLM/SOFTWARE/Microsoft/Windows NT/CurrentVersion" and then we'll hook 
   the reg_printing backend onto the last component of the path (see 
   KEY_PRINTING_2K in include/rpc_reg.h)   --jerry */

static const char *builtin_registry_paths[] = {
	KEY_PRINTING_2K,
	KEY_PRINTING_PORTS,
	KEY_PRINTING,
	KEY_SHARES,
	KEY_EVENTLOG,
	KEY_SMBCONF,
	"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib",
	"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib\\009",
	"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Print\\Monitors",
	"HKLM\\SYSTEM\\CurrentControlSet\\Control\\ProductOptions",
	"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Terminal Server\\DefaultUserConfiguration",
	"HKLM\\SYSTEM\\CurrentControlSet\\Services\\TcpIp\\Parameters",
	"HKLM\\SYSTEM\\CurrentControlSet\\Services\\Netlogon\\Parameters",
	"HKU",
	"HKCR",
	"HKPD",
	"HKPT",
	 NULL };

struct builtin_regkey_value {
	const char *path;
	const char *valuename;
	uint32 type;
	union {
		const char *string;
		uint32 dw_value;
	} data;
};

static struct builtin_regkey_value builtin_registry_values[] = {
	{ KEY_PRINTING_PORTS,
		SAMBA_PRINTER_PORT_NAME, REG_SZ, { "" } },
	{ KEY_PRINTING_2K,
		"DefaultSpoolDirectory", REG_SZ, { "C:\\Windows\\System32\\Spool\\Printers" } },
	{ KEY_EVENTLOG,
		"DisplayName", REG_SZ, { "Event Log" } }, 
	{ KEY_EVENTLOG,
		"ErrorControl", REG_DWORD, { (char*)0x00000001 } },
	{ NULL, NULL, 0, { NULL } }
};

/***********************************************************************
 Open the registry data in the tdb
 ***********************************************************************/
 
static bool init_registry_data( void )
{
	pstring path, base, remaining;
	fstring keyname, subkeyname;
	REGSUBKEY_CTR *subkeys;
	REGVAL_CTR *values;
	int i;
	const char *p, *p2;
	UNISTR2 data;

	/*
	 * There are potentially quite a few store operations which are all
	 * indiviually wrapped in tdb transactions. Wrapping them in a single
	 * transaction gives just a single transaction_commit() to actually do
	 * its fsync()s. See tdb/common/transaction.c for info about nested
	 * transaction behaviour.
	 */

	if ( tdb_transaction_start( tdb_reg->tdb ) == -1 ) {
		DEBUG(0, ("init_registry_data: tdb_transaction_start "
			  "failed\n"));
		return False;
	}
	
	/* loop over all of the predefined paths and add each component */
	
	for ( i=0; builtin_registry_paths[i] != NULL; i++ ) {

		DEBUG(6,("init_registry_data: Adding [%s]\n", builtin_registry_paths[i]));

		pstrcpy( path, builtin_registry_paths[i] );
		pstrcpy( base, "" );
		p = path;
		
		while ( next_token(&p, keyname, "\\", sizeof(keyname)) ) {
		
			/* build up the registry path from the components */
			
			if ( *base )
				pstrcat( base, "\\" );
			pstrcat( base, keyname );
			
			/* get the immediate subkeyname (if we have one ) */
			
			*subkeyname = '\0';
			if ( *p ) {
				pstrcpy( remaining, p );
				p2 = remaining;
				
				if ( !next_token(&p2, subkeyname, "\\", sizeof(subkeyname)) )
					fstrcpy( subkeyname, p2 );
			}

			DEBUG(10,("init_registry_data: Storing key [%s] with subkey [%s]\n",
				base, *subkeyname ? subkeyname : "NULL"));
			
			/* we don't really care if the lookup succeeds or not since
			   we are about to update the record.  We just want any 
			   subkeys already present */
			
			if ( !(subkeys = TALLOC_ZERO_P( NULL, REGSUBKEY_CTR )) ) {
				DEBUG(0,("talloc() failure!\n"));
				goto fail;
			}

			regdb_fetch_keys( base, subkeys );
			if ( *subkeyname ) 
				regsubkey_ctr_addkey( subkeys, subkeyname );
			if ( !regdb_store_keys( base, subkeys ))
				goto fail;
			
			TALLOC_FREE( subkeys );
		}
	}

	/* loop over all of the predefined values and add each component */
	
	for ( i=0; builtin_registry_values[i].path != NULL; i++ ) {
		if ( !(values = TALLOC_ZERO_P( NULL, REGVAL_CTR )) ) {
			DEBUG(0,("talloc() failure!\n"));
			goto fail;
		}

		regdb_fetch_values( builtin_registry_values[i].path, values );

		/* preserve existing values across restarts.  Only add new ones */

		if ( !regval_ctr_key_exists( values, builtin_registry_values[i].valuename ) ) 
		{
			switch( builtin_registry_values[i].type ) {
			case REG_DWORD:
				regval_ctr_addvalue( values, 
				                     builtin_registry_values[i].valuename,
						     REG_DWORD,
						     (char*)&builtin_registry_values[i].data.dw_value,
						     sizeof(uint32) );
				break;
				
			case REG_SZ:
				init_unistr2( &data, builtin_registry_values[i].data.string, UNI_STR_TERMINATE);
				regval_ctr_addvalue( values, 
				                     builtin_registry_values[i].valuename,
						     REG_SZ,
						     (char*)data.buffer,
						     data.uni_str_len*sizeof(uint16) );
				break;
			
			default:
				DEBUG(0,("init_registry_data: invalid value type in builtin_registry_values [%d]\n",
					builtin_registry_values[i].type));
			}
			regdb_store_values( builtin_registry_values[i].path, values );
		}
		
		TALLOC_FREE( values );
	}
	
	if (tdb_transaction_commit( tdb_reg->tdb ) == -1) {
		DEBUG(0, ("init_registry_data: Could not commit "
			  "transaction\n"));
		return False;
	}

	return True;

 fail:

	if (tdb_transaction_cancel( tdb_reg->tdb ) == -1) {
		smb_panic("init_registry_data: tdb_transaction_cancel "
			  "failed\n");
	}

	return False;
}

/***********************************************************************
 Open the registry database
 ***********************************************************************/
 
bool regdb_init( void )
{
	const char *vstring = "INFO/version";
	uint32 vers_id;

	if ( tdb_reg )
		return True;

	if ( !(tdb_reg = tdb_wrap_open(NULL, lock_path("registry.tdb"), 0, REG_TDB_FLAGS, O_RDWR, 0600)) )
	{
		tdb_reg = tdb_wrap_open(NULL, lock_path("registry.tdb"), 0, REG_TDB_FLAGS, O_RDWR|O_CREAT, 0600);
		if ( !tdb_reg ) {
			DEBUG(0,("regdb_init: Failed to open registry %s (%s)\n",
				lock_path("registry.tdb"), strerror(errno) ));
			return False;
		}
		
		DEBUG(10,("regdb_init: Successfully created registry tdb\n"));
	}

	tdb_refcount = 1;

	vers_id = tdb_fetch_int32(tdb_reg->tdb, vstring);

	if ( vers_id != REGVER_V1 ) {
		/* any upgrade code here if needed */
		DEBUG(10, ("regdb_init: got INFO/version = %d != %d\n",
			   vers_id, REGVER_V1));
	}

	/* always setup the necessary keys and values */

	if ( !init_registry_data() ) {
		DEBUG(0,("init_registry: Failed to initialize data in registry!\n"));
		return False;
	}

	return True;
}

/***********************************************************************
 Open the registry.  Must already have been initialized by regdb_init()
 ***********************************************************************/

WERROR regdb_open( void )
{
	WERROR result = WERR_OK;

	if ( tdb_reg ) {
		DEBUG(10,("regdb_open: incrementing refcount (%d)\n", tdb_refcount));
		tdb_refcount++;
		return WERR_OK;
	}
	
	become_root();

	tdb_reg = tdb_wrap_open(NULL, lock_path("registry.tdb"), 0, REG_TDB_FLAGS, O_RDWR, 0600);
	if ( !tdb_reg ) {
		result = ntstatus_to_werror( map_nt_error_from_unix( errno ) );
		DEBUG(0,("regdb_open: Failed to open %s! (%s)\n", 
			lock_path("registry.tdb"), strerror(errno) ));
	}

	unbecome_root();

	tdb_refcount = 1;
	DEBUG(10,("regdb_open: refcount reset (%d)\n", tdb_refcount));

	return result;
}

/***********************************************************************
 ***********************************************************************/

int regdb_close( void )
{
	tdb_refcount--;

	DEBUG(10,("regdb_close: decrementing refcount (%d)\n", tdb_refcount));

	if ( tdb_refcount > 0 )
		return 0;

	SMB_ASSERT( tdb_refcount >= 0 );

	TALLOC_FREE(tdb_reg);
	return 0;
}

/***********************************************************************
 return the tdb sequence number of the registry tdb.
 this is an indicator for the content of the registry
 having changed. it will change upon regdb_init, too, though.
 ***********************************************************************/
int regdb_get_seqnum(void)
{
	return tdb_get_seqnum(tdb_reg->tdb);
}

/***********************************************************************
 Add subkey strings to the registry tdb under a defined key
 fmt is the same format as tdb_pack except this function only supports
 fstrings
 ***********************************************************************/
 
static bool regdb_store_keys_internal( const char *key, REGSUBKEY_CTR *ctr )
{
	TDB_DATA dbuf;
	uint8 *buffer;
	int i = 0;
	uint32 len, buflen;
	bool ret = True;
	uint32 num_subkeys = regsubkey_ctr_numkeys( ctr );
	pstring keyname;
	
	if ( !key )
		return False;

	pstrcpy( keyname, key );
	normalize_reg_path( keyname );

	/* allocate some initial memory */
		
	if (!(buffer = (uint8 *)SMB_MALLOC(sizeof(pstring)))) {
		return False;
	}
	buflen = sizeof(pstring);
	len = 0;
	
	/* store the number of subkeys */
	
	len += tdb_pack(buffer+len, buflen-len, "d", num_subkeys );
	
	/* pack all the strings */
	
	for (i=0; i<num_subkeys; i++) {
		len += tdb_pack( buffer+len, buflen-len, "f", regsubkey_ctr_specific_key(ctr, i) );
		if ( len > buflen ) {
			/* allocate some extra space */
			if ((buffer = (uint8 *)SMB_REALLOC( buffer, len*2 )) == NULL) {
				DEBUG(0,("regdb_store_keys: Failed to realloc memory of size [%d]\n", len*2));
				ret = False;
				goto done;
			}
			buflen = len*2;
					
			len = tdb_pack( buffer+len, buflen-len, "f", regsubkey_ctr_specific_key(ctr, i) );
		}		
	}
	
	/* finally write out the data */
	
	dbuf.dptr = buffer;
	dbuf.dsize = len;
	if ( tdb_store_bystring( tdb_reg->tdb, keyname, dbuf, TDB_REPLACE ) == -1) {
		ret = False;
		goto done;
	}

done:		
	SAFE_FREE( buffer );
	
	return ret;
}

/***********************************************************************
 Store the new subkey record and create any child key records that 
 do not currently exist
 ***********************************************************************/

bool regdb_store_keys( const char *key, REGSUBKEY_CTR *ctr )
{
	int num_subkeys, i;
	pstring path;
	REGSUBKEY_CTR *subkeys = NULL, *old_subkeys = NULL;
	char *oldkeyname;
	
	/*
	 * fetch a list of the old subkeys so we can determine if anything has
	 * changed
	 */

	if ( !(old_subkeys = TALLOC_ZERO_P( ctr, REGSUBKEY_CTR )) ) {
		DEBUG(0,("regdb_store_keys: talloc() failure!\n"));
		goto fail;
	}

	regdb_fetch_keys( key, old_subkeys );

	if (ctr->num_subkeys == old_subkeys->num_subkeys) {

		for (i = 0; i<ctr->num_subkeys; i++) {
			if (strcmp(ctr->subkeys[i],
				   old_subkeys->subkeys[i]) != 0) {
				break;
			}
		}
		if (i == ctr->num_subkeys) {
			/*
			 * Nothing changed, no point to even start a tdb
			 * transaction
			 */
			TALLOC_FREE(old_subkeys);
			return True;
		}
	}

	if ( tdb_transaction_start( tdb_reg->tdb ) == -1 ) {
		DEBUG(0, ("regdb_store_keys: tdb_transaction_start failed\n"));
		return False;
	}

	/*
	 * Re-fetch the old keys inside the transaction
	 */

	TALLOC_FREE(old_subkeys);

	if ( !(old_subkeys = TALLOC_ZERO_P( ctr, REGSUBKEY_CTR )) ) {
		DEBUG(0,("regdb_store_keys: talloc() failure!\n"));
		goto fail;
	}

	regdb_fetch_keys( key, old_subkeys );
	
	/* store the subkey list for the parent */
	
	if ( !regdb_store_keys_internal( key, ctr ) ) {
		DEBUG(0,("regdb_store_keys: Failed to store new subkey list "
			 "for parent [%s]\n", key ));
		goto fail;
	}
	
	/* now delete removed keys */
	
	num_subkeys = regsubkey_ctr_numkeys( old_subkeys );
	for ( i=0; i<num_subkeys; i++ ) {
		oldkeyname = regsubkey_ctr_specific_key( old_subkeys, i );

		if ( regsubkey_ctr_key_exists( ctr, oldkeyname ) ) {
			/*
			 * It's still around, don't delete
			 */

			continue;
		}

		pstr_sprintf( path, "%s/%s", key, oldkeyname );
		normalize_reg_path( path );
		if (tdb_delete_bystring( tdb_reg->tdb, path ) == -1) {
			DEBUG(1, ("Deleting %s failed\n", path));
			goto fail;
		}
		
		pstr_sprintf( path, "%s/%s/%s", REG_VALUE_PREFIX, key,
			      oldkeyname );
		normalize_reg_path( path );

		/*
		 * Ignore errors here, we might have no values around
		 */
		tdb_delete_bystring( tdb_reg->tdb, path );
	}

	TALLOC_FREE( old_subkeys );
	
	/* now create records for any subkeys that don't already exist */
	
	num_subkeys = regsubkey_ctr_numkeys( ctr );
	for ( i=0; i<num_subkeys; i++ ) {
		pstr_sprintf( path, "%s/%s", key,
			      regsubkey_ctr_specific_key( ctr, i ) );

		if ( !(subkeys = TALLOC_ZERO_P( ctr, REGSUBKEY_CTR )) ) {
			DEBUG(0,("regdb_store_keys: talloc() failure!\n"));
			goto fail;
		}

		if ( regdb_fetch_keys( path, subkeys ) == -1 ) {
			/* create a record with 0 subkeys */
			if ( !regdb_store_keys_internal( path, subkeys ) ) {
				DEBUG(0,("regdb_store_keys: Failed to store "
					 "new record for key [%s]\n", path ));
				goto fail;
			}
		}

		TALLOC_FREE( subkeys );
	}

	if (tdb_transaction_commit( tdb_reg->tdb ) == -1) {
		DEBUG(0, ("regdb_store_keys: Could not commit transaction\n"));
		return False;
	}

	return True;

 fail:
	TALLOC_FREE( old_subkeys );
	TALLOC_FREE( subkeys );

	if (tdb_transaction_cancel( tdb_reg->tdb ) == -1) {
		smb_panic("regdb_store_keys: tdb_transaction_cancel failed\n");
	}

	return False;
}


/***********************************************************************
 Retrieve an array of strings containing subkeys.  Memory should be 
 released by the caller.  
 ***********************************************************************/

int regdb_fetch_keys( const char* key, REGSUBKEY_CTR *ctr )
{
	pstring path;
	uint32 num_items;
	TDB_DATA dbuf;
	uint8 *buf;
	uint32 buflen, len;
	int i;
	fstring subkeyname;

	DEBUG(11,("regdb_fetch_keys: Enter key => [%s]\n", key ? key : "NULL"));
	
	pstrcpy( path, key );
	
	/* convert to key format */
	pstring_sub( path, "\\", "/" ); 
	strupper_m( path );
	
	dbuf = tdb_fetch_bystring( tdb_reg->tdb, path );
	
	buf = dbuf.dptr;
	buflen = dbuf.dsize;
	
	if ( !buf ) {
		DEBUG(5,("regdb_fetch_keys: tdb lookup failed to locate key [%s]\n", key));
		return -1;
	}
	
	len = tdb_unpack( buf, buflen, "d", &num_items);
	
	for (i=0; i<num_items; i++) {
		len += tdb_unpack( buf+len, buflen-len, "f", subkeyname );
		regsubkey_ctr_addkey( ctr, subkeyname );
	}

	SAFE_FREE( dbuf.dptr );
	
	DEBUG(11,("regdb_fetch_keys: Exit [%d] items\n", num_items));
	
	return num_items;
}

/****************************************************************************
 Unpack a list of registry values frem the TDB
 ***************************************************************************/
 
static int regdb_unpack_values(REGVAL_CTR *values, uint8 *buf, int buflen)
{
	int 		len = 0;
	uint32		type;
	pstring		valuename;
	uint32		size;
	uint8		*data_p;
	uint32 		num_values = 0;
	int 		i;
	
	
	
	/* loop and unpack the rest of the registry values */
	
	len += tdb_unpack(buf+len, buflen-len, "d", &num_values);
	
	for ( i=0; i<num_values; i++ ) {
		/* unpack the next regval */
		
		type = REG_NONE;
		size = 0;
		data_p = NULL;
		len += tdb_unpack(buf+len, buflen-len, "fdB",
				  valuename,
				  &type,
				  &size,
				  &data_p);
				
		/* add the new value. Paranoid protective code -- make sure data_p is valid */

		if ( size && data_p ) {
			regval_ctr_addvalue( values, valuename, type, (const char *)data_p, size );
			SAFE_FREE(data_p); /* 'B' option to tdb_unpack does a malloc() */
		}

		DEBUG(8,("specific: [%s], len: %d\n", valuename, size));
	}

	return len;
}

/****************************************************************************
 Pack all values in all printer keys
 ***************************************************************************/
 
static int regdb_pack_values(REGVAL_CTR *values, uint8 *buf, int buflen)
{
	int 		len = 0;
	int 		i;
	REGISTRY_VALUE	*val;
	int		num_values;

	if ( !values )
		return 0;

	num_values = regval_ctr_numvals( values );

	/* pack the number of values first */
	
	len += tdb_pack( buf+len, buflen-len, "d", num_values );
	
	/* loop over all values */
		
	for ( i=0; i<num_values; i++ ) {			
		val = regval_ctr_specific_value( values, i );
		len += tdb_pack(buf+len, buflen-len, "fdB",
				regval_name(val),
				regval_type(val),
				regval_size(val),
				regval_data_p(val) );
	}

	return len;
}

/***********************************************************************
 Retrieve an array of strings containing subkeys.  Memory should be 
 released by the caller.
 ***********************************************************************/

int regdb_fetch_values( const char* key, REGVAL_CTR *values )
{
	TDB_DATA data;
	pstring keystr;

	DEBUG(10,("regdb_fetch_values: Looking for value of key [%s] \n", key));
	
	pstr_sprintf( keystr, "%s/%s", REG_VALUE_PREFIX, key );
	normalize_reg_path( keystr );
	
	data = tdb_fetch_bystring( tdb_reg->tdb, keystr );
	
	if ( !data.dptr ) {
		/* all keys have zero values by default */
		return 0;
	}
	
	regdb_unpack_values( values, data.dptr, data.dsize );
	
	SAFE_FREE( data.dptr );
	
	return regval_ctr_numvals(values);
}

bool regdb_store_values( const char *key, REGVAL_CTR *values )
{
	TDB_DATA old_data, data;
	pstring keystr;
	int len, ret;
	
	DEBUG(10,("regdb_store_values: Looking for value of key [%s] \n", key));
	
	ZERO_STRUCT( data );
	
	len = regdb_pack_values( values, data.dptr, data.dsize );
	if ( len <= 0 ) {
		DEBUG(0,("regdb_store_values: unable to pack values. len <= 0\n"));
		return False;
	}
	
	data.dptr = SMB_MALLOC_ARRAY( uint8, len );
	data.dsize = len;
	
	len = regdb_pack_values( values, data.dptr, data.dsize );
	
	SMB_ASSERT( len == data.dsize );
	
	pstr_sprintf( keystr, "%s/%s", REG_VALUE_PREFIX, key );
	normalize_reg_path( keystr );
	
	old_data = tdb_fetch_bystring(tdb_reg->tdb, keystr);

	if ((old_data.dptr != NULL)
	    && (old_data.dsize == data.dsize)
	    && (memcmp(old_data.dptr, data.dptr, data.dsize) == 0)) {
		SAFE_FREE(old_data.dptr);
		SAFE_FREE(data.dptr);
		return True;
	}

	ret = tdb_trans_store_bystring(tdb_reg->tdb, keystr, data, TDB_REPLACE);
	
	SAFE_FREE( old_data.dptr );
	SAFE_FREE( data.dptr );
	
	return ret != -1 ;
}

static WERROR regdb_get_secdesc(TALLOC_CTX *mem_ctx, const char *key,
				struct security_descriptor **psecdesc)
{
	char *tdbkey;
	TDB_DATA data;
	NTSTATUS status;

	DEBUG(10, ("regdb_get_secdesc: Getting secdesc of key [%s]\n", key));

	if (asprintf(&tdbkey, "%s/%s", REG_SECDESC_PREFIX, key) == -1) {
		return WERR_NOMEM;
	}
	normalize_dbkey(tdbkey);

        data = tdb_fetch_bystring(tdb_reg->tdb, tdbkey);
	SAFE_FREE(tdbkey);

	if (data.dptr == NULL) {
		return WERR_BADFILE;
	}

	status = unmarshall_sec_desc(mem_ctx, (uint8 *)data.dptr, data.dsize,
				     psecdesc);

	SAFE_FREE(data.dptr);

	if (NT_STATUS_EQUAL(status, NT_STATUS_NO_MEMORY)) {
		return WERR_NOMEM;
	}

	if (!NT_STATUS_IS_OK(status)) {
		return WERR_REG_CORRUPT;
	}

	return WERR_OK;
}

static WERROR regdb_set_secdesc(const char *key,
				struct security_descriptor *secdesc)
{
	prs_struct ps;
	TALLOC_CTX *mem_ctx;
	char *tdbkey;
	WERROR err = WERR_NOMEM;
	TDB_DATA tdbdata;

	if (!(mem_ctx = talloc_init("regdb_set_secdesc"))) {
		return WERR_NOMEM;
	}

	ZERO_STRUCT(ps);

	if (!(tdbkey = talloc_asprintf(mem_ctx, "%s/%s", REG_SECDESC_PREFIX,
				       key))) {
		goto done;
	}
	normalize_dbkey(tdbkey);

	err = ntstatus_to_werror(marshall_sec_desc(mem_ctx, secdesc,
						   &tdbdata.dptr,
						   &tdbdata.dsize));
	if (!W_ERROR_IS_OK(err)) {
		goto done;
	}

	if (tdb_trans_store_bystring(tdb_reg->tdb, tdbkey, tdbdata, 0) == -1) {
		err = ntstatus_to_werror(map_nt_error_from_unix(errno));
		goto done;
	}

 done:
	prs_mem_free(&ps);
	TALLOC_FREE(mem_ctx);
	return err;
}

/* 
 * Table of function pointers for default access
 */
 
REGISTRY_OPS regdb_ops = {
	regdb_fetch_keys,
	regdb_fetch_values,
	regdb_store_keys,
	regdb_store_values,
	NULL,
	regdb_get_secdesc,
	regdb_set_secdesc
};
