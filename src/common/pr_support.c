/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-1995 Patrick Powell, San Diego State University
 *     papowell@sdsu.edu
 * See LICENSE for conditions of use.
 *
 ***************************************************************************
 * MODULE: pr_support.c
 * PURPOSE: Printer opening
 **************************************************************************/

static char *const _id =
"$Id: pr_support.c,v 3.3 1996/08/31 21:11:58 papowell Exp papowell $";

/***************************************************************************
 Commentary:
Patrick Powell, Fri May 19 08:01:34 PDT 1995
This code is taken almost directly from the PLp_device Version 4.0 code.
Additional sanity checks and cleanups have been added.

 ***************************************************************************
 * MODULE: Print_support.c
 * handle the actual output to the Printer
 ***************************************************************************
 * int Print_open( int timeout ): opens the Printer
 * int Print_close( int timeout ): closes the Printer
 * int Print_ready(): combines Print_open() and Print_of_fd()
 * int of_stop(): stops the 'of' filter
 * int Print_string( str, len ) : prints a string through 'of' or to Printer
 * int Print_copy( fd) : copies a file through 'of' or to Printer
 * int Print_filter( file, cmd) : makes a filter and copies file
 * int Print_banner(): prints a banner through 'of' or to Printer
 * Note: the above procedures which return values return JSUCC on success,
 *   and JFAIL or JABORT on failure
 ***************************************************************************/

#include "lp.h"
#include "printcap.h"
#include "pr_support.h"
#include "timeout.h"
#include "decodestatus.h"

/***************************************************************************
 * int Print_open( struct filter *filter, struct control_file *cfp,
 *   int timeout, int interval, int max_try )
 *
 * 	Open the Printer, and set the filter structure with the pid and output
 *   device.
 * 	If the RW printcap flag is set, output is opened RW, otherwise
 * 	opened writeonly in append mode.
 *
 * Side Effect:
 *   terminates if Printer is unable to be opened
 ****************************************************************************/

int Print_open( struct filter *filter,
	struct control_file *cfp, int timeout, int interval, int grace,
	int max_try, struct pc_used *pc_used, int accounting_port )
{
	static int attempt = 0, err;
	struct stat statb;
	static int device_fd;
	time_t tm;
	char tm_str[32];
	char *s, *end;

	DEBUG5( "Print_open: device '%s', max_try %d, interval %d, timeout %d",
		Lp_device, max_try, interval, timeout );
	time( &tm );
	tm_str[0] = 0;
	safestrncat( tm_str, Time_str(1,tm) );
	if( Lp_device == 0 || *Lp_device == 0 ){
		fatal(LOG_ERR, "Print_open: printer '%s' missing Lp_device value",
			Printer );
	}
	if( (s = strchr( Lp_device, '%' )) ){
		/* we have a host%port form */
		*s++ = 0;
		end = s;
		Destination_port = strtol( s, &end, 10 );
		if( ((end - s) != strlen( s )) || Destination_port < 0 ){
		fatal( LOG_ERR,
			"Print_open: 'lp' entry has bad port number '%s'",
				Lp_device );
		}
	} else if( Lp_device[0] != '|' && Lp_device[0] != '/' ){
		fatal(LOG_ERR, "Print_open: printer '%s', bad 'lp' entry '%s'", 
			Printer, Lp_device );
	}

	if (grace) {
		setstatus( cfp, "Waiting Grace period '%s' : '%d'",
			Lp_device, grace);
		sleep(grace);
	}

	/* set flag for closing on timeout */
	do{
		setstatus( cfp,
		"opening '%s' at %s, attempt %d",
			Lp_device, tm_str, ++attempt);

		filter->pid = 0;
		filter->input = 0;
		switch( Lp_device[0] ){
		case '|':
			device_fd = DevNullFD;
			if( device_fd <= 0 ){
				logerr_die( LOG_CRIT, "Print_open: bad DevNullFD");
			} 
			Make_filter( 'f', cfp, filter,
				Lp_device+1, 1, Read_write, device_fd, pc_used, (void *)0,
				accounting_port, Logger_destination != 0 );
			err = errno;
			close(device_fd);
			device_fd = filter->input;
			break;
		case '/':
			device_fd = -1;
			if( Set_timeout( timeout, 0 ) ){
				device_fd = Checkwrite( Lp_device, &statb, 
					(Lock_it || Read_write)
						?(O_RDWR):(O_APPEND|O_WRONLY), 0, Nonblocking_open );
			}
			err = errno;
			Clear_timeout();

			/* Note: there is no timeout race condition here -
				we either set the device_fd or we did not */
			if( Lock_it && device_fd >= 0 ){
				/*
				 * lock the device so that multiple servers can
				 * use it
				 */
				if( isatty( device_fd ) ){
					if( LockDevice( device_fd, Lp_device ) <= 0 ){
						err = errno;
						setstatus( cfp,
							"Device '%s' is locked!", Lp_device );
						close( device_fd );
						device_fd = -1;
					}
				} else if( S_ISREG(statb.st_mode ) ){
					if( Do_lock( device_fd, Lp_device, 1 ) <= 0 ){
						err = errno;
						setstatus( cfp,
							"Device '%s' is locked!", Lp_device );
						close( device_fd );
						device_fd = -1;
					}
				}
			}
			filter->input = device_fd;
			break;
		default:
			device_fd = Link_open( Lp_device, max_try, timeout);
			err = errno;
			filter->input = device_fd;
			break;
		}

		if( device_fd < 0 ){
			errno = err;
			DEBUG4( "Print_open: cannot open device '%s' - '%s'",
				Lp_device, Errormsg(err) );
			if( (max_try == 0 || attempt < max_try) && interval > 0 ){
				setstatus( cfp, "Cannot open '%s' - '%s', sleeping %d",
					Lp_device, Errormsg( err), interval );
				sleep( interval );
			} else {
				break;
			}
		}
	} while( device_fd < 0 );


	if( device_fd < 0 ){
		setstatus( cfp, "cannot open '%s' after %d attempts - %s",
			Lp_device, attempt, Errormsg(err) );
		Errorcode = JFAIL;
	} else if( isatty( device_fd ) ){
		/*
		 * if it is a tty, set the baud rates and control information;
		 * Note that the open O_NCTTY prevents this from being a control
		 *  device for the process.
		 */
		Do_stty( device_fd );
	}
	filter->input = device_fd;
	DEBUG7 ("Print_open: %s is fd %d", Lp_device, device_fd);
	return( device_fd );
}


/***************************************************************************
 * Print_flush()
 * 1. Flush all of the open pipe and/or descriptors
 ***************************************************************************/
void Print_flush( void )
{
	DEBUG5("Print_flush: flushing printers");
	Flush_filter( &Device_fd_info );
	Flush_filter( &OF_fd_info );
	Flush_filter( &XF_fd_info );
	Flush_filter( &Pr_fd_info );
	Flush_filter( &Af_fd_info );
	Flush_filter( &As_fd_info );
}

/***************************************************************************
 * Print_close( int timeout )
 * 1. Close all of the open pipe and/or descriptors
 * 2. Signal the filter processes to start up
 * 3. We wait for a while for them to complete
 ***************************************************************************/

void Print_close( int timeout )
{
	DEBUG5("Print_close: closing printers");
	Print_flush();
	/* shut them all down first */
	Close_filter( &Device_fd_info, 0 );
	Close_filter( &OF_fd_info, 0 );
	Close_filter( &XF_fd_info, 0 );
	Close_filter( &Pr_fd_info, 0 );
	Close_filter( &Af_fd_info, 0 );
	Close_filter( &As_fd_info, 0 );
	/* now wait for them to complete */
	Close_filter( &Device_fd_info, timeout );
	Close_filter( &OF_fd_info, timeout );
	Close_filter( &XF_fd_info, timeout );
	Close_filter( &Pr_fd_info, timeout );
	Close_filter( &Af_fd_info, timeout );
	Close_filter( &As_fd_info, timeout );
}

/***************************************************************************
 * Print_kill( signal )
 * 1. Flush all of the open pipe and/or descriptors
 * 2. Close all of the open pipe and/or descriptors
 * 3. Kill them off if necessary
 ***************************************************************************/

void Print_kill( int signal )
{
	DEBUG5("Print_kill: killing filters");
	Print_flush();
	Kill_filter( &Device_fd_info, signal );
	Kill_filter( &OF_fd_info, signal );
	Kill_filter( &XF_fd_info, signal );
	Kill_filter( &Pr_fd_info, signal );
	Kill_filter( &Af_fd_info, signal );
	Kill_filter( &As_fd_info, signal );
}

/***************************************************************************
 * Print_abort()
 * 1. Close all of the open pipe and/or descriptors
 * 2. Wait for all the filter processes to terminate
 * 3. After timeout period, kill them off
 * 
 ***************************************************************************/

void Print_abort( void )
{
	pid_t result;
	plp_status_t status;
	int step = 0;
	int err;


	DEBUG8( "Print_abort: starting shutdown" );

	/* be gentle at first */
	Print_close( 1 );

	DEBUG8( "Print_abort: killing children" );
	do{
		do{
			status = 0;
			result = plp_waitpid( -1, &status, WNOHANG );
			err = errno;
			DEBUG8( "Print_abort: result %d, status 0x%x", result, status );
			removepid( result );
		} while( result > 0 );
		if( result == 0 || (result == -1 && err != ECHILD) ){
			DEBUG8( "Print_abort: step %d, killing", step, status );
			switch( step++ ){
				case 0: Print_kill( SIGINT ); break;	/* hit it once */
				case 1: Print_kill( SIGQUIT ); break;   /* hit it harder */
				case 2: Print_kill( SIGKILL ); break;   /* hit it really hard */
				case 3: return; /* give up */
			}
			sleep(1);
		}
	}while( result == -1 && err != ECHILD );
	DEBUG8( "Print_abort: done" );
}

int of_start( struct filter *filter )
{
	DEBUG2 ("of_start: sending SIGCONT to OF_Filter (pid %d)", filter->pid );
	if( kill( filter->pid, SIGCONT) < 0 ){
		logerr( LOG_WARNING, "cannot restart output filter");
		return( JFAIL);
	}
	return JSUCC;
}

/***************************************************************************
 * of_stop()
 * wait for the output filter to stop itself
 * Return: JSUCC if stopped, JFAIL if it died
 ***************************************************************************/

static char filter_stop[] = "\031\001";		/* magic string to stop OF_Filter */

int of_stop( struct filter *filter, int timeout )
{
	static int pid = 0;
	int err;
	plp_status_t statb;
	DEBUG3 ("of_stop: writing stop string to fd %d, pid %d",
		filter->input, filter->pid );
	if( Print_string( filter, filter_stop, strlen( filter_stop ),
			timeout ) != JSUCC ){
		return( JFAIL );
	}

	DEBUG3 ("of_stop: waiting for OF_Filter %d", filter->pid );
	statb = 0;
	do{
		if( Set_timeout( timeout, 0 ) ){
			pid = plp_waitpid( filter->pid, &statb, WUNTRACED ); /**/
		}
		Clear_timeout();
		err = errno;
		DEBUG8( "of_stop: pid %d, statb 0x%x", pid, statb );
	}while( Alarm_timed_out == 0 && pid == -1 && err != ECHILD );

	if( Alarm_timed_out || !WIFSTOPPED (statb) ){
		logerr( LOG_WARNING, "of_stop: did not stop or died (%s)",
				Decode_status( &statb));
		return( JFAIL);
	}
	DEBUG3 ("of_stop: output filter stopped");

	return( JSUCC);
}


/***************************************************************************
 * Print_string( struct filter, char *str, int len, int timeout )
 * print a string through a filter
 * 1. Enable the line Printer
 * 2. get the filter or device fd;
 * 3. put out the string
 * 4. if unsuccessful, close the Printer
 * Return: JSUCC if successful, JFAIL otherwise
 ***************************************************************************/

int Print_string( struct filter *filter, char *str, int len, int timeout )
{
	static int i = 0;		/* ACME again */
	int err;

	if( str == 0 || len <= 0 ){
		return( JSUCC);
	}
	if( Set_timeout( timeout, 0 ) ){
		i = Write_fd_len( filter->input, str, len );
	}
	Clear_timeout();
	err = errno;
	if( Alarm_timed_out || i < 0 ){
		if( Alarm_timed_out ){
			log( LOG_WARNING, "Print_string: write timeout error");
		} else {
			logerr( LOG_WARNING, "Print_string: write error - %s",
				Errormsg(err) );
		}
		return( JFAIL);
	}
	return( JSUCC);
}


/***************************************************************************
 * Print_copy( int fd, struct filter *filter, int timeout )
 * copy a file through a filter
 * 1. copy the file to the appropriate output device
 * 2. if an error, close the Printer
 ***************************************************************************/

int Print_copy( struct control_file *cfp, int fd, struct stat *statb,
	struct filter *filter, int timeout, int status )
{
	char buf[LARGEBUFFER];
	int in;                     /* bytes read */
	static int t = 0;						/* amount written */
	static int total = 0;
	int err;					/* errno status */
	static int fraction;				/* 25 % of file size */
	static int oldfraction;			/* how much done */
	int quanta = 4;				/* resolution of amount */

	DEBUG4("Print_copy: fd %d to fd %d, pid %d, timeout %d, size %d",
		fd, filter->input, filter->pid, timeout, statb->st_size );

	
	fraction = statb->st_size/quanta;
	oldfraction = 0;
	if( fraction == 0 ){
		fraction = 1;
	}
	while( (in = read( fd, buf, sizeof( buf) ) ) > 0 ){
		DEBUG7("Print_copy: read %d, total %d", in, total );
		if( (t = total/fraction) != oldfraction ){
			oldfraction = t;
			setstatus( cfp, "printed %d%% of %d bytes",
				(int)( (total*100)/statb->st_size ), statb->st_size ); 
		}
		if( Set_timeout( timeout, 0 ) ){
			t = Write_fd_len( filter->input, buf, in );
		}
		Clear_timeout();
		err = errno;

		DEBUG7("Print_copy: printed %d", t );
		if( Alarm_timed_out || t < 0 ){
			if( Alarm_timed_out ){
				if( status ){
					setstatus( cfp, "timeout after writing %d bytes",
					total ); 
				}
				logerr( LOG_WARNING,
					"Print_copy: write error timeout after %d bytes");
			} else {
				if( status ){
					setstatus( cfp, "write error after %d bytes '%s'",
					total, Errormsg(err) );
				}
				errno = err;
				logerr( LOG_WARNING, "write error after %d bytes", total );
			}
			return( JFAIL);
		}
		total += in;
	}
	/*
	 * completed the reading
	 */
	err = errno;
	if( in < 0 ){
		if( status ){
			setstatus( cfp, "read error after '%d' bytes, '%s'",
			total, Errormsg(err) );
		}
		errno = err;
		logerr( LOG_WARNING, "Print_copy: read error");
		return( JFAIL);
	}
	if( status )setstatus( cfp, "printed all %d bytes", total ); 

	DEBUG3("Print_copy: printed %d bytes", total);
	return( JSUCC);
}
