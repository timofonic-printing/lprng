/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-1995 Patrick Powell, San Diego State University
 *     papowell@sdsu.edu
 * See LICENSE for conditions of use.
 *
 ***************************************************************************
 * MODULE: printjob.c
 * PURPOSE: print a file
 **************************************************************************/

static char *const _id =
"$Id: printjob.c,v 3.8 1996/09/09 14:24:41 papowell Exp papowell $";

#include "lp.h"
#include "lp_config.h"
#include "printcap.h"
#include "pr_support.h"
#include "jobcontrol.h"
#include "decodestatus.h"
#include "jobcontrol.h"

/***************************************************************************
Commentary:
Patrick Powell Sat May 13 08:24:43 PDT 1995

The following algorithm is used to print a job

dev_fd - device file descriptor
   This can be the actual device or a pipe to a device
of_fd  - output file descriptor
   This is the OF filter if it has been specified.
   This is used for banners and file separators such as form feeds.
if_fd  - 
   This is the file format filter.

dev_fd = open( device or filter );

do accounting at start;
if( OF ){
	of_fd = open( OF filter -> dev_fd );
} else {
    of_fd = dev_fd;
}

  now we put out the various initialization strings

Leader_on_open -> of_fd;
FF_on_open     -> of_fd;
if( ( Always_banner || !Suppress_bannner) && !Banner_last ){
	banner = bp;
	if( bs ) banner = bs;
	bannner -> of_fd;
	kill off banner printer;
	wait for banner printer to exit;
}

  now we suspend the of filter
if( of_fd != dev_fd ){
	Suspend_string->of_fd;
	wait for suspend;
}

 print out the data files
for( i = 0; i < data_files; ++i ){
	if( IF ){
		if_fd = open( IF filter -> of_fd )
	} else {
		if_fd = dev_fd;
	}
	file -> if_fd;
	if( if_fd != dev_fd ){
		close( if_fd );
		wait for  if_filter to die;
	}
}

 start up the of filter
if( of_fd != dev_fd ){
	wait up of_filter;
}

 put out the termination stuff
if( ( Always_banner || !Suppress_bannner) && Banner_last ){
	banner = bp;
	if( be ) banner = be;
	bannner -> of_fd;
	kill off banner printer;
	wait for banner printer to exit;
}

Trailer_on_close -> of_fd;
FF_on_close     -> of_fd;
close( of_fd );
do accounting at end;
close( dev_fd );
if( of_fd != dev_fd ){
	wait for of_filter;
}

****************************************************************************/

static int Fix_str( char **copy, char *str );
static int setup_exit;
static int Print_banner( char *cmd, struct control_file *cfp,
	int timeout, int ff, char *str, struct pc_used *pc_used );

static char *Pr_temp;

static void Remove_pr_temp()
{
	if( Pr_temp && *Pr_temp ){
		unlink(Pr_temp);
		Pr_temp = 0;
	}
}

int Print_job( struct control_file *cfp, struct pc_used *pc_used )
{
	int FF_len;				/* FF information length */
	static char *FF_str;	/* FF information converted format */
	int leader_len;			/* leader */
	static char *leader_str;
	int trailer_len;		/* trailer */
	static char *trailer_str;
	struct data_file *data_file;	/* current data file */
	int fd;					/* data file file descriptor */
	struct stat statb;		/* stat buffer for open */
	char *filter;			/* filter to use */
	pid_t result;			/* waitpid resutl and status */
	plp_status_t status;
	int i, c, n, err;		/* ACME Integer variables */
	char *pname;			/* pname */
	char *s;				/* Sigh... */
	int do_banner;			/* do a banner */
	int pfd;				/* pr program output */

	DEBUG2("Print_job: '%s', Use_queuename %d", cfp->name, Use_queuename );

	if( Use_queuename && cfp->QUEUENAME == 0 && Printer ){
		char buffer[M_QUEUENAME];
		plp_snprintf( buffer, sizeof(buffer), "Q%s", Printer );
		cfp->QUEUENAME = Prefix_job_line( cfp, buffer );
	}
	if( Use_identifier && cfp->IDENTIFIER == 0 ){
		cfp->IDENTIFIER = Prefix_job_line( cfp, cfp->identifier-1 );
	}

	/* we will not try to do anything fancy on exit except kill filters */
	if( setup_exit == 0 ){
		register_exit( (exit_ret)Print_close, (void *)1 );
		register_exit( (exit_ret)Remove_pr_temp, (void *)1 );
		setup_exit = 1;
	}

	Errorcode = JABORT;
	Device_fd_info.input = -1;
	OF_fd_info.input = -1;
	XF_fd_info.input = -1;
	Pr_fd_info.input = -1;


	/* fix the form feed string */
	FF_len = Fix_str( &FF_str, Form_feed );
	leader_len = Fix_str( &leader_str, Leader_on_open );
	trailer_len = Fix_str( &trailer_str, Trailer_on_close );

	if( Debug > 3 ){
		dump_control_file( "Print_job", cfp );
	}

	if( Local_accounting ){
		Setup_accounting( cfp, pc_used );
	}

	/*
	 * if( OF ){
	 * 	of_fd = open( OF filter -> dev_fd );
	 * } else {
	 * 	of_fd = dev_fd;
	 * }
	 */

	setstatus(cfp,"printing '%s'",cfp->name);
	if( Print_open( &Device_fd_info, cfp, Connect_timeout, Connect_interval,
		Connect_grace, Connect_try, pc_used, Accounting_port ) < 0 ){
		/* we failed to open device */
		Errorcode = JFAIL;
		cleanup( 0 );
	}
	DEBUG8("Print_job: device fd %d, pid %d",
		Device_fd_info.input, Device_fd_info.pid );
	Errorcode = JABORT;
	if( OF_Filter ){
		/* we need to create an OF filter */
		Make_filter( 'o', cfp, &OF_fd_info, OF_Filter, 0, 0,
			Device_fd_info.input, pc_used, (void *)0, Accounting_port, Logger_destination != 0 );
	} else {
		OF_fd_info.input = Device_fd_info.input;
		OF_fd_info.pid = 0;
	}
	if( OF_fd_info.input == -1 ){
		if( Errorcode == 0 ){
			log( LOG_ERR,
				"Print_job: OF_fd_input has bad status, JSUCC errorcode" );
			Errorcode = JABORT;
		}
		cleanup(0);
	}

	/*
	 * do accounting at start
	 */
	if( Accounting_start && Local_accounting ){
		setstatus(cfp,"accounting at start '%s'", cfp->name );
		i = Do_accounting( 0, Accounting_start, cfp, Send_timeout, pc_used, OF_fd_info.input );
		if( i ){
			setstatus(cfp,"accounting failed at start '%s' - %s", cfp->name, Server_status(i) );
			if( i != JFAIL && i != JABORT && i != JREMOVE ){
				i = JFAIL;
			}
			Errorcode = i;
			cleanup( 0 );
		}
	}

	/* Leader_on_open -> of_fd; */
	if( leader_len ){
		setstatus(cfp,"printing '%s', sending leader",cfp->name);
		i = Print_string( &OF_fd_info, leader_str, leader_len,
			Send_timeout );
		if( i ){
			setstatus(cfp,"printing '%s', error sending leader",cfp->name);
			n = Close_filter( &OF_fd_info, Send_timeout );
			if( n ) i = n;
			Errorcode = i;
			cleanup( 0 );
		}
	}

	/* FF_on_open     -> of_fd; */
	if( FF_on_open && FF_len ){
		setstatus(cfp,"printing '%s', sending FF on open",cfp->name);
		i = Print_string( &OF_fd_info, FF_str, FF_len,
			Send_timeout );
		if( i ){
			setstatus(cfp,"printing '%s', error sending FF on open",cfp->name);
			n = Close_filter( &OF_fd_info, Send_timeout );
			if( n ) i = n;
			Errorcode = i;
			cleanup( 0 );
		}
	}

	/*
	 * if( ( Always_banner || !Suppress_bannner) && !Banner_last ){
	 *  we need to have a banner and a banner name
	 * 	bannner -> of_fd;
	 * 	kill off banner printer;
	 * 	wait for banner printer to exit;
	 * }
	 */

	/* we are always going to do a banner; get the user name */
	if( Always_banner && cfp->BNRNAME == 0 ){
			cfp->BNRNAME = cfp->LOGNAME;
			if( cfp->BNRNAME == 0 ){
				cfp->BNRNAME = "ANONYMOUS";
			}
	}

	/* check for the banner printing */
	do_banner = !Suppress_header && cfp->BNRNAME;

	/* now we have a banner, is it at start or end? */
	if( do_banner && ( Banner_start || !Banner_last ) ){
		setstatus(cfp,"printing '%s', printing banner on open",cfp->name);
		s = 0;
		DEBUG6( "Print_job: Banner_start '%s', Banner_printer '%s', Default '%s'",
			Banner_start, Banner_printer, Default_banner_printer );
		if( Default_banner_printer && *Default_banner_printer ){
			s = Default_banner_printer;
		}
		if( Banner_printer && *Banner_printer ) s = Banner_printer;
		if( Banner_start && *Banner_start ) s = Banner_start;
		i = Print_banner( s, cfp, Send_timeout, FF_len, FF_str, pc_used );
		if( i ){
			setstatus(cfp,"printing '%s', error printing banner on open",cfp->name);
			Errorcode = i;
			cleanup( 0 );
		}
	}

	/* 
	 *   now we suspend the of filter
	 * if( of_fd != dev_fd ){
	 * 	Suspend_string->of_fd;
	 * 	wait for suspend;
	 * }
	 */

	if( OF_fd_info.input != Device_fd_info.input
		&& (i = of_stop( &OF_fd_info, Send_timeout ) ) ){
		setstatus(cfp,"printing '%s', error suspending OF filter",cfp->name);
		n = Close_filter( &OF_fd_info, Send_timeout );
		if( n ) i = n;
		Errorcode = i;
		cleanup( 0 );
	}

	/* 
	 *  print out the data files
	 * for( i = 0; i < data_files; ++i ){
	 *  if( i > 0 ) FF separator -> of_fd;
	 * 	if( IF ){
	 * 		if_fd = open( IF filter -> of_fd )
	 * 	} else {
	 * 		if_fd = dev_fd;
	 * 	}
	 * 	file -> if_fd;
	 * 	if( if_fd != dev_fd ){
	 * 		close( if_fd );
	 * 		wait for  if_filter to die;
	 * 	}
	 * }
	 */

	for( i = 0; i < cfp->data_file.count; ++i ){
		data_file = (void *)cfp->data_file.list;
		data_file = &data_file[i];

		if( i && !No_FF_separator && FF_len ){
			/* FF separator -> of_fd; */
			setstatus(cfp,"printing '%s', FF separator ",cfp->name);
			n = 0;
			if( OF_fd_info.input != Device_fd_info.input ){
				n = of_start( &OF_fd_info );
			}
			if( !n ){
				n = Print_string( &OF_fd_info, FF_str, FF_len,
					Send_timeout );
			}
			if( !n && OF_fd_info.input != Device_fd_info.input ){
				n = of_stop( &OF_fd_info, Send_timeout );
			}
			if( n ){
				Errorcode = n;
				n = Close_filter( &OF_fd_info, Send_timeout );
				if( n ) Errorcode = n;
				cleanup( 0 );
			}
		}

		setstatus(cfp,"printing job '%s data file '%s', format '%c'",
			cfp->name, data_file->openname, data_file->format );
		pname = Add_path( SDpathname, data_file->openname );
		fd = Checkread( pname, &statb );
		if( fd < 0 ){
			setstatus(cfp,"cannot open data file '%s'",data_file->openname);
			fatal( LOG_ERR, "Print_job: job '%s', cannot open data file '%s'",
				cfp->name, data_file->openname );
		}
		if( (statb.st_size != data_file->statb.st_size)
			|| (statb.st_ctime != data_file->statb.st_ctime) ){
			setstatus(cfp,"data file '%s' changed during printing",
				data_file->openname);
			Errorcode = JABORT;
			fatal( LOG_CRIT,
				"Print_job: data file '%s' changed during printing!!",
				data_file->openname );
		}
		/*
		 * check for PR to be used
		 */
		c = data_file->format;
		if( 'p' == c ){
			char pr_filter[SMALLBUFFER];
			if( Pr_program == 0 || *Pr_program  == 0 ){
				setstatus(cfp,"no 'p' format filter available" );
				Errorcode = JREMOVE;
				fatal( LOG_ERR, "Print_job: no '-p' formatter for '%s'",
					c, data_file->openname );
			}
			pr_filter[0] = 0;
			safestrncat( pr_filter, Pr_program );
			safestrncat( pr_filter, " $w $l" );
			if( cfp->PRTITLE ) safestrncat( pr_filter, " -h $-T" );
			/* we need to open a file and process the job */
			s = strrchr( pname, '/' );
			if( s == 0 ){
				Errorcode = JABORT;
				fatal( LOG_ERR, "Print_job: bad pathname to job file",
					pname );
			}
			s[1] = 'B';
			Pr_temp = pname;
			/* make a temp file */
			pfd = Checkwrite( pname, &statb, O_RDWR, 1, 0 );
			DEBUG8( "Print_job: pr temp file '%s', fd %d", pname, pfd );
			if( ftruncate( pfd, 0 )  < 0 ){
				Errorcode = JABORT;
				logerr_die( LOG_ERR, "Print_job: ftruncate of %s failed",
					pname );
			}
			/* make the pr program */
			Make_filter( 'f', cfp, &Pr_fd_info, pr_filter, 1, 0,
				pfd, pc_used, data_file, Accounting_port,
				Logger_destination != 0 );
			/* filter the data file */
			err = Print_copy(cfp, fd, &data_file->statb, &Pr_fd_info,
				Send_timeout, 1);
			n = Close_filter( &Pr_fd_info, Send_timeout );
			DEBUG8( "Print_job: pr filter status  %d", n );
			if( n ){
				Errorcode = n;
				cleanup(0);
			}
			/* unlink the temporary file */
			Remove_pr_temp();
			/* save the file descriptor */
			if( dup2( pfd, fd ) < 0 ){
				Errorcode = JABORT;
				logerr_die( LOG_ERR, "Print_job: dup2 failed" );
			}
			if( lseek( fd, 0, SEEK_SET ) == (off_t)(-1) ){
				Errorcode = JABORT;
				logerr_die( LOG_ERR, "Print_job: lseek failed" );
			}
		}
		/*
		 * now we check to see if there is an input filter
		 */
		filter = 0;
		switch( c ){
		case 'p': case 'f': case 'l':
			filter = IF_Filter;
			c = 'f';
			break;
		default:
			filter = Find_filter( c, pc_used );
			if( filter == 0 ){
				Errorcode = JREMOVE;
				setstatus(cfp,"no '%c' format filter available", c );
				fatal( LOG_ERR, "Print_job: cannot find filter '%c' for '%s'",
					c, data_file->openname );
			}
		}
		/* hook filter up to the device file descriptor */
		if( filter ){
			Make_filter( c, cfp, &XF_fd_info, filter, 0, 0,
				Device_fd_info.input, pc_used, data_file, Accounting_port, Logger_destination != 0 );
		} else {
			XF_fd_info.input = Device_fd_info.input;
			XF_fd_info.pid = 0;
		}
		/* send job to printer - we get the error status of filter if any */
		n = 0;
		err = Print_copy(cfp, fd, &data_file->statb, &XF_fd_info, Send_timeout, 1);
		(void)close(fd);
		/* close Xf */
		n = Close_filter( &XF_fd_info, Send_timeout );
		if( n ) err = n;
		/* exit with either the filter status or the copy status */
		if( err ){
			Errorcode = err;
			cleanup(0);
		}
	}

	/* 
	 *  start up the of filter
	 * if( of_fd != dev_fd ){
	 * 	wait up of_filter;
	 * }
	 */

	if( OF_fd_info.input != Device_fd_info.input ){
		DEBUG8( "Print_job: restarting OF filter" );
		i = of_start( &OF_fd_info );
		if( i ){
			DEBUG8( "Print_job: restarting OF filter FAILED, result %d", i );
			n = Close_filter( &OF_fd_info, Send_timeout );
			if( n ) i = n;
			Errorcode = i;
			cleanup( 0 );
		}
	}

	/* 
	 *  put out the termination stuff
	 * if( ( Always_banner || !Suppress_bannner) && Banner_last ){
	 * 	bannner -> of_fd;
	 * 	kill off banner printer;
	 * 	wait for banner printer to exit;
	 * }
	 */ 


	if( do_banner && ( Banner_end || Banner_last ) ){
		if( !No_FF_separator && FF_len ){
			/* FF befor banner     -> of_fd; */
			setstatus(cfp,"printing '%s', FF separator ",cfp->name);
			DEBUG8( "Print_job: printing FF separator" );
			i = Print_string( &OF_fd_info, FF_str, FF_len,
				Send_timeout );
			if( i ){
				DEBUG8( "Print_job: printing FF separator FAILED, status 0x%x", i );
				Errorcode = i;
				cleanup( 0 );
			}
		}
		setstatus(cfp,"printing '%s', printing banner on close",cfp->name);
		s = 0;
		DEBUG6( "Print_job: Banner_end '%s', Banner_printer '%s', Default '%s'",
			Banner_end, Banner_printer, Default_banner_printer );
		if( Default_banner_printer && *Default_banner_printer ){
			s = Default_banner_printer;
		}
		if( Banner_printer && *Banner_printer ) s = Banner_printer;
		if( Banner_end && *Banner_end ) s = Banner_end;
		DEBUG8( "Print_job: printing banner" );
		i = Print_banner( s, cfp, Send_timeout, FF_len, FF_str, pc_used );
		if( i ){
			DEBUG8( "Print_job: printing banner FAILED, status 0x%x", i );
			Errorcode = i;
			cleanup( 0 );
		}
	}

	/* 
	 * FF_on_close     -> of_fd;
	 */ 
	if( FF_on_close && FF_len ){
		setstatus(cfp,"printing '%s', sending FF on close",cfp->name);
		DEBUG8( "Print_job: printing FF on close" );
		i = Print_string( &OF_fd_info, FF_str, FF_len, Send_timeout );
		if( i ){
			DEBUG8( "Print_job: printing FF on close FAILED, status 0x%x", i );
			Errorcode = i;
			cleanup( 0 );
		}
	}

	/* 
	 * Trailer_on_close -> of_fd;
	 */ 
	if( trailer_len ){
		setstatus(cfp,"printing '%s', sending trailer",cfp->name);
		DEBUG8( "Print_job: printing trailer" );
		i = Print_string( &OF_fd_info, trailer_str,trailer_len,
			Send_timeout);
		if( i ){
			DEBUG8( "Print_job: printing trailer FAILED, status 0x%x", i );
			Errorcode = i;
			cleanup( 0 );
		}
	}


	/*
	 * do accounting at end
	 */
	if( Accounting_end && Local_accounting ){
		setstatus(cfp,"accounting at end '%s'", cfp->name );
		DEBUG8( "Print_job: accounting at end" );
		i = Do_accounting( 1, Accounting_end, cfp, Send_timeout, pc_used,
			OF_fd_info.input );
		if( i ){
			setstatus(cfp,"accounting failed at end '%s'", cfp->name );
			DEBUG8( "Print_job: accounting at end FAILED, status 0x%x", i );
			if( i != JFAIL && i != JABORT && i != JREMOVE ){
				i = JFAIL;
			}
			Errorcode = i;
			cleanup(0);
		}
	}

	/*
	 * close( of_fd );
	 */
	setstatus(cfp,"job '%s', closing device",cfp->name);
	n = Close_filter( &OF_fd_info, Send_timeout );
	if( n ){
		DEBUG8( "Print_job: close OF filter FAILED, status 0x%x", n );
		Errorcode = n;
		cleanup( 0 );
	}

	/* we should wait for exit of all print facilities */
	Print_close( Send_timeout );

	do{
		status = 0;
		result = plp_waitpid( -1, &status, 0 );
		DEBUG8( "Print_job: waiting for child, result %d, status 0x%x",
			result, status );
		removepid( result );
	} while( status == 0 && result != -1 && errno != ECHILD );
	if( status ){
		if( WIFSIGNALED( status ) ){
			Errorcode = JABORT;
		} else {
			Errorcode = WEXITSTATUS( status );
		}
	} else {
		Errorcode = JSUCC;
	}

	setstatus(cfp,"job '%s' finished ",cfp->name);
	return( Errorcode );
}

/***************************************************************************
 * int Fix_str( char **copy, *str );
 * - make a copy of the original string
 * - substitute all the escape characters
 * \f, \n, \r, \t, and \nnn
 ***************************************************************************/
static int Fix_str( char **copy, char *str )
{
	int len, c;
	char *s, *t, *end;

	if( *copy ){
		free( *copy );
		*copy = 0;
	}
	if( str && *str ){
		*copy = safestrdup(str);
	}
	DEBUG9("Fix_str: '%s'", str );
	for( len = 0, s = *copy, t = str; t && (c = (s[len] = *t++)); ++len ){
		/* DEBUG9("Fix_str: char '%c', len %d", c, len ); */
		if( c == '\\' && (c = *t) != '\\' ){
			/* check for \nnn */
			if( isdigit( c ) ){
				end = t;
				s[len] = strtol( t, &end, 8 );
				if( (end - t ) != 3 ){
					return( -1 );
				}
				t = end;
			} else {
				switch( c ){
					default: s[len] = c; break;
					case 'f': s[len] = '\f'; break;
					case 'r': s[len] = '\r'; break;
					case 'n': s[len] = '\n'; break;
					case 't': s[len] = '\t'; break;
					case 0: return(-1);
				}
				++t;
			}
		}
		/* DEBUG9("Fix_str: result '0x%x', len %d", s[len], len ); */
	}
	if( Debug > 8 ){
		logDebug( "Fix_str: str '%s' -> ", str );
		for( c = 0; c < len; ++c ){
			logDebug( "Fix_str: [%d] 0x%x", c, s[c] );
		}
	}
	return( len );
}

/*
 * Print a banner
 * check for a small or large banner as necessary
 */

static int Print_banner(
	char *banner_printer,
	struct control_file *cfp, int timeout,
	int FF_len, char *FF_str, struct pc_used *pc_used )
{
	char bline[SMALLBUFFER];
	char *ep;
	int i;


	/*
	 * print the banner
	 */
	DEBUG4( "Printer_banner: banner_printer '%s'", banner_printer );

	/* make short banner look like BSD-style( Sun, etc.) short banner.
	 * 
	 * This is necessary for use with an OF that parses
	 * the banner( as lprps does).
	 */
	
	if( Banner_line == 0 ){
		Banner_line = "";
	}
	DEBUG4( "Printer_banner: raw banner '%s'", Banner_line );
	ep = bline + sizeof(bline) - 2;
	ep = Expand_command( cfp, bline, ep, Banner_line, 'f', (void *)0 );
	i = strlen(bline);
	DEBUG4( "Printer_banner: expanded banner '%s'", bline );
	bline[i++] = '\n';
	bline[i++] = 0;


 	if( !Short_banner && banner_printer && *banner_printer ){
		Make_filter( 'f', cfp, &Pr_fd_info, banner_printer, 0, 0,
			OF_fd_info.input, pc_used, (void *)0, Accounting_port, Logger_destination != 0 );
	} else {
		Pr_fd_info.input = OF_fd_info.input;
		Pr_fd_info.pid = 0;
	}

	DEBUG4( "Printer_banner: writing short banner '%s'", bline );
	if( Write_fd_str( Pr_fd_info.input, bline ) < 0 ){
		logerr( LOG_INFO, "banner: write to banner printer failed");
		Errorcode = JFAIL;
		cleanup( 0 );
	}
	if( Pr_fd_info.input != OF_fd_info.input ){
		i = Close_filter( &Pr_fd_info, timeout );
		if( i ){
			Errorcode = i;
			cleanup( 0 );
		}
	}
	if( !No_FF_separator && FF_len ){
		/* FF after banner     -> of_fd; */
		setstatus(cfp,"printing '%s', sending FF after banner",cfp->name);
		i = Print_string( &OF_fd_info, FF_str, FF_len,
			Send_timeout );
		if( i ){
			Errorcode = i;
			cleanup( 0 );
		}
	}
	return( JSUCC );
}

void Setup_accounting( struct control_file *cfp, struct pc_used *pc_used )
{
	int oldport, i, n;
	struct stat statb;
	char *s;

	DEBUG6("Setup_accounting: '%s'", Accounting_file);
	s = Accounting_file;
	if( s == 0 || *s == 0 ) return;
	if( s[0] == '|' ){
		/* first try to make the filter */
		Accounting_file = 0;
		Make_filter( 'f', cfp, &Af_fd_info, s, 0, 0,
			0, pc_used, (void *)0, 0, Logger_destination != 0 );
		Accounting_port = Af_fd_info.input;
	} else if( (s = strchr( s, '%' )) ){
		/* now try to open a connection to a server */
		i = 0;
		if( s ){
			i = atoi( s+1 );
		}
		if( s == 0 || i <= 0 ){
			logerr( LOG_ERR,
				"Setup_accounting: bad Accounting server info '%s'", Accounting_file );
			plp_snprintf( cfp->error, sizeof(cfp->error),
				"bad accounting server '%s'", Accounting_file );
			Set_job_control( cfp );
			Errorcode = JFAIL;
			cleanup(0);
		}
		*s = 0;
		oldport = Destination_port;
		Destination_port = i;
		DEBUG6("Setup_accounting: opening connection to %s port %d", Accounting_file, i );
		n = Link_open( Accounting_file, Connect_try, Connect_timeout );
		DEBUG6("Setup_accounting: socket %d", Accounting_port );
		*s = '%';
		Destination_port = oldport;
		if( n < 0 ){
			const char *t;
			i = errno;
			t = Errormsg(errno);
			DEBUG6("Setup_accounting: connection failed to %s '%s'",
				Accounting_file, t );
			errno = i;
			logerr( LOG_ERR,
				"Do_accounting: cannot connect to accounting server" );
			plp_snprintf( cfp->error, sizeof(cfp->error),
				"accounting server '%s' unavailable '%s'",
					Accounting_file, t );
			Set_job_control( cfp );
			Errorcode= JFAIL;
			cleanup( 0 );
		}
		Af_fd_info.input = Accounting_port = n;
		Accounting_file = 0;
	} else {
		n = Checkwrite( Accounting_file, &statb, 0, 0, 0 );
		if( n > 0 ){
			Af_fd_info.input = Accounting_port = n;
		}
	}
}

int Do_accounting( int end, char *command, struct control_file *cfp,
	int timeout, struct pc_used *pc_used, int filter_out )
{
	int i, err;
	char *ep;
	char buffer[SMALLBUFFER];

	if( command ) while( isspace( *command ) ) ++command;

	DEBUG6("Do_accounting: %s", command );

	err = JSUCC;
	if( command == 0 ){
		;
	} else if( *command == '|' ){
		Make_filter( 'f', cfp, &As_fd_info, command, 0, 0,
			filter_out, pc_used, (void *)0, Accounting_port, Logger_destination != 0 );
		err = Close_filter( &As_fd_info, timeout );
		DEBUG6("Do_accounting: filter exit status %s", Server_status(err) );
		if( err ){
			plp_snprintf( cfp->error, sizeof(cfp->error),
				"accounting check at %s failed - status %s", end?"end":"start",
				Server_status( err ) );
			setstatus( cfp,  cfp->error );
			Set_job_control( cfp );
		}
	} else if( Accounting_port > 0 ){
		/* now we have to expand the string */
		ep = buffer + sizeof(buffer) - 2;
		ep = Expand_command( cfp, buffer, ep, command, 'f', (void *)0 );
		i = strlen(buffer);
		buffer[i++] = '\n';
		buffer[i++] = 0;
		DEBUG4("Do_accounting: job '%s' '%s'", cfp->name, buffer );
		if( Write_fd_str( Accounting_port, buffer ) <  0 ){
			err = JFAIL;
			logerr( LOG_ERR, "Do_accounting: write failed" );
			plp_snprintf( cfp->error, sizeof(cfp->error),
				"accounting write failed" );
			err = JFAIL;
			Set_job_control( cfp );
		} else if( Accounting_check ){
			static char accept[] = "accept";
			i = sizeof(buffer) - 1;
			buffer[0] = 0;
			err = Link_line_read( "ACCOUNTING SERVER", &Accounting_port,
				Send_timeout, buffer, &i );
			buffer[i] = 0;
			if( err ){
				plp_snprintf( cfp->error, sizeof( cfp->error ),
					"read failed from accounting server" );
				err = JFAIL;
				Set_job_control( cfp );
			} else if( strncasecmp( buffer, accept, sizeof(accept)-1 ) ){
				logerr( LOG_ERR,
					"Do_accounting: accounting check failed '%s'", buffer );
				plp_snprintf( cfp->error, sizeof(cfp->error),
					"accounting check failed '%s'", buffer );
				err = JFAIL;
				Set_job_control( cfp );
			}
		}
	}
	return( err );
}
