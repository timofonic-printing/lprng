/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-1995 Patrick Powell, San Diego State University
 *     papowell@sdsu.edu
 * See LICENSE for conditions of use.
 *
 ***************************************************************************
 * MODULE: checpc.c
 * PURPOSE: check printcap for problems
 **************************************************************************/

#include "checkpc.h"
#include "lp_config.h"
#include "setuid.h"
#include "checkpc_perm.h"

static char *const _id =
"$Id: checkpc_perm.c,v 3.0 1996/05/19 04:05:33 papowell Exp $";

/***************************************************************************
 Commentary
 Patrick Powell Sun Apr 30 07:04:27 PDT 1995
	We need to check to see if we have a spool directory.  If we do not,
	then we need to create one.
	1. we stat all of the entries in the path
	2. if one is missing,  and fix != 0, we change to ROOT and try
		to create it;  we then chown the directory to daemon;
	3. the final spool directory is owned by daemon;

 ***************************************************************************/

static int gid, uid;

int Check_perms( struct dpathname *dpath, int fix, int age, int remove )
{
	struct dirent *d;
	DIR *dir;
	time_t t;
	char *s;

	/* get the required group and user ids */
	t = time( (void *)0 );
	if( gid == 0 ){
		gid = Getdaemon_group();
		uid = Getdaemon();
	}

	DEBUG7("Check_perms: '%s'", dpath->pathname );

	/* now we check on files in the directory */
	s = Clear_path( dpath );
	dir = opendir( s );
	if( dir == 0 ){
		Warnmsg( "opendir '%s' failed, %s", s, Errormsg( errno ) );
		return( 1 );
	}

	/* now we read the files */
	/* strcmp is safe */


	while( (d = readdir(dir)) ){
		DEBUG4("Check_perms: entry '%s'", d->d_name );
		if( strcmp( d->d_name, "." ) == 0
			|| strcmp( d->d_name, ".." ) == 0 ) continue;

		s = Add_path( dpath, d->d_name );
		if( check_file( dpath, fix, t, age, remove ) ){
			closedir(dir);
			return( 1 );
		}
	}
	closedir(dir);
	return( 0 );
}

/* check to see that the file name is for a job */
static int is_job_file( char *s )
{
	if( (islower( s[0] ) || s[0] == '_' )
		&& s[1] == 'f' 
		&& isalpha( s[2] )
		&& isdigit( s[3] )
		&& isdigit( s[4] )
		&& isdigit( s[5] ) 
		){
		return( 1 );
	}
	return( 0 );
}

/***************************************************************************
 * check_file( struct dpathname *dpath   - pathname of directory/files
 *    int fix  - fix or check
 *    int t    - time to compare against
 *    int age  - maximum age of file
 ***************************************************************************/
int check_file( struct dpathname *dpath,
	int fix, time_t t, int age, int remove )
{
	struct stat statb;
	char *path, *s;
	int old;
	int err;

	path = dpath->pathname;

	DEBUG8("check_file: '%s', fix %d, time 0x%x, age %d",
		path, fix, t, age );

	if( stat( path, &statb ) ){
		err = errno;
		Warnmsg( "cannot stat file '%s', %s", path, Errormsg(errno) );
		errno = err;
		return( 1 );
	}
	if( S_ISDIR( statb.st_mode ) ){
		struct dpathname newpath;
		Init_path( &newpath, path );
		return( Check_perms( &newpath, fix, age, remove ) );
	} else if( !S_ISREG( statb.st_mode ) ){
		Warnmsg( "'%s' not a regular file - unusual", path );
		return( 0 );
	}
	if( statb.st_uid != uid || statb.st_gid != gid ){
		if( fix ){
			if( fix_owner( path ) ) return(1);
		} else {
			Warnmsg( "owner/group of '%s' are %d/%d, not %d/%d", path,
				statb.st_uid, statb.st_gid, uid, gid );
			return( 1 );
		}
	}
	if( 077777 & (statb.st_mode ^ Spool_file_perms) ){
		if( fix ){
			if( fix_perms( path, Spool_file_perms ) ) return(1);
		} else {
			Warnmsg( "permissions of '%s' are 0%o, not 0%o", path,
				statb.st_mode & 077777, Spool_file_perms );
			return( 1 );
		}
	}
	if( age ){
		s = strrchr( path, '/' );
		if( s ){
			s = s+1;
		} else {
			s = path;
		}
		/* check to see if the file has the format specified */
		if( is_job_file( s ) ){
			/* check on the age */
			old = t - statb.st_ctime;
			if( old >= age ){
				fprintf( stdout,
					"file %s age is %d secs, max allowed %d secs\n",
					s, old, age );
				if( remove ){
					fprintf( stdout, "removing '%s'\n", path );
					if( unlink( path ) == -1 ){
						Warnmsg( "cannot remove '%s', %s", path,
							Errormsg(errno) );
					}
				}
			}
		}
	}
	return( 0 );
}

int fix_create_dir( struct dpathname *dpath, struct stat *statb )
{
	char *s, *end, *path;
	int mask, err, status;
	struct dpathname p;

	p = *dpath;
	path = p.pathname;
	Warnmsg( "creating '%s'", path );
	end = 0;
	for( end = 0, s = path;s && *s; s = end+1 ){
		if( end ) *end = '/';
		end = strchr( s, '/' );
		if( end ){
			*end = 0;
		}
		if( strlen( s ) == 0 ){
			continue;
		}
		DEBUG7("fix_create_dir: creating %s", path );
		if( stat( path, statb ) == 0 ){
			if( S_ISDIR( statb->st_mode ) ){
				continue;
			}
			if( !S_ISREG( statb->st_mode ) ){
				Warnmsg( "not regular file '%s'", path );
				return( 1 );
			}
			if( unlink( s ) ){
				Warnmsg( "cannot unlink file '%s', %s",
					path, Errormsg(errno) );
				return(1);
			}
		}
		/* we don't have a directory */
		mask = umask( 0 );
		To_root();
		status = mkdir( path, Spool_dir_perms );
		err = errno;
		To_daemon();
		(void)umask( mask );
		errno = err;
		if( status ){
			Warnmsg( "mkdir '%s' failed, %s", path, Errormsg(err) );
			return( 1 );
		}
		if( fix_owner( path ) ) return( 1 );
	}
	return( 0 );
}

int fix_owner( char *path )
{
	int status;
	int err;

	Warnmsg( "changing ownership '%s'", path );

	DEBUG7("change ownership: '%s' to %d/%d", path, uid, gid );
	To_root();
	status =  chown( path, uid, gid );
	err = errno;
	To_daemon();
	if( status ){
		Warnmsg( "chown '%s' failed, %s", path, Errormsg(err) );
	}
	errno = err;
	return( status != 0 );
}

int fix_perms( char *path, int perms )
{
	int status;
	int err;

	To_root();
	status = chmod( path, perms );
	err = errno;
	To_daemon();

	if( status ){
		Warnmsg( "chmod '%s' to 0%o failed, %s", path, perms,
			 Errormsg(err) );
	}
	errno = err;
	return( status != 0 );
}


/***************************************************************************
 * int Check_spool_dir( struct dpathname *dpath, int fix )
 * Check to see that the spool directory exists, and create it if necessary
 ***************************************************************************/

int Check_spool_dir( struct dpathname *dpath, int fix )
{
	struct stat statb;
	char *pathname;
	int err;

	/* get the required group and user ids */
	if( gid == 0 ){
		gid = Getdaemon_group();
		uid = Getdaemon();
	}

	
	pathname = Clear_path( dpath );
	DEBUG7("Check_spool_dir: '%s'", pathname );

	if( stat( pathname, &statb ) < 0 ){
		if( fix ){
			if( fix_create_dir( dpath, &statb ) )return(2);
		} else {
			return( 2 );
		}
	}
	if( !S_ISDIR( statb.st_mode ) ){
		DEBUG7("Check_spool_dir: not directory '%s'", pathname );
		if( fix ){
			if( fix_create_dir( dpath, &statb ) ) return( 1 );
		} else {
			return( 1 );
		}
	}
	if( statb.st_uid != uid || statb.st_gid != gid ){
		if( fix ){
			if( fix_owner( pathname ) ) return( 1 );
			if( stat( pathname, &statb ) ){
				err = errno;
				Warnmsg( "cannot stat '%s', %s", pathname, Errormsg(err) );
				errno = err;
				return( 1 );
			}
		} else {
			Warnmsg( "owner/group of '%s' are %d/%d, not %d/%d", pathname,
				statb.st_uid, statb.st_gid, uid, gid );
		}
	}
	if( 077777 & (statb.st_mode ^ Spool_dir_perms) ){
		if( fix ){
			if( fix_perms( pathname, Spool_dir_perms ) ) return(1);
			if( stat( pathname, &statb ) ){
				err = errno;
				Warnmsg( "cannot stat '%s', %s", pathname, Errormsg(err) );
				errno = err;
				return( 1 );
			}
		} else {
			Warnmsg( "permissions of '%s' are 0%o, not 0%o", pathname,
				statb.st_mode & 077777, Spool_dir_perms );
		}
	}
	return(0);
}
