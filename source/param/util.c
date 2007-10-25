/* 
   Unix SMB/CIFS implementation.
   Loadparm utility functions
   Copyright (C) Jelmer Vernooij 2007
   
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


/*****************************************************************
 A useful function for returning a path in the Samba lock directory.
*****************************************************************/  

char *lock_path(const char *name)
{
	pstring fname;

	pstrcpy(fname,lp_lockdir());
	trim_char(fname,'\0','/');
	
	if (!directory_exist(fname,NULL))
		mkdir(fname,0755);
	
	pstrcat(fname,"/");
	pstrcat(fname,name);

	return talloc_strdup(talloc_tos(), fname);
}


