/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-2001, Patrick Powell, San Diego, CA
 *     papowell@lprng.com
 * See LICENSE for conditions of use.
 *
 ***************************************************************************/

 static char *const _id =
"$Id: printjob.c,v 1.23 2001/09/29 22:28:52 papowell Exp $";


#include "lp.h"
#include "errorcodes.h"
#include "printjob.h"
#include "getqueue.h"
#include "child.h"
#include "fileopen.h"
/**** ENDINCLUDE ****/
#if defined(HAVE_TCDRAIN)
#  if defined(HAVE_TERMIOS_H)
#    include <termios.h>
#  endif
#endif

/***************************************************************************
 * Commentary:
 * Patrick Powell Sat May 13 08:24:43 PDT 1995
 * 
 * The following algorithm is used to print a job
 * 
 * The 'Send_through OF_filter' operation does the following:
 *  if the of filter process does not exist then we create it
 *  if the 'suspend_of_filter' flag is true then we add the suspend
 *     string to the buffer
 *  we send the buffer to the of filter processes
 *  if the 'suspend_of_filter' flag is false or the 'end of job' flag
 *    is true then we wait for the filter to exit.
 * 
 *   now we put out the various initialization strings
 * 
 * Leader_on_open_DYN -> buffer;
 * FF_on_open_DYN     -> buffer;
 * if( ( Always_banner_DYN || !Suppress_banner) && !Banner_last_DYN ){
 * 	banner -> buffer
 * }
 * 
 * if( OF_FILTER ) buffer-> Send_through_of_filter
 * else buffer -> output
 * 
 *  print out the data files
 * for( i = 0; i < data_files; ++i ){
 *     if( i > 0 && FF between files && OF Filter ){
 * 		FF -> buffer
 *      if( OF_FILTER ) buffer-> Send_through_of_filter
 *      else buffer -> output
 *     }
 *    filter = lookup filter
 *    if( filter ) Send_file_through_filter( filter )
 *    else file ->output
 * }
 * 
 * if( (Always_banner_DYN || !Suppress_banner) && Banner_last_DYN ){
 * 	banner -> buffer;
 * }
 * Trailer_on_close_DYN -> buffer;
 * FF_on_close_DYN     -> buffer;
 * 
 * if( OF_FILTER ) buffer-> Send_through_of_filter(end_of_job as well)
 * else buffer -> output
 * 
 ****************************************************************************/

 
/****************************************************************************
 * int Print_job( int output, - output device
 *   int status_device - read status from this device is > 0
 *   struct job *job   - job to print
 *   int timeout       - timeout for job - 0 is no timeout
 *   int poll_for_status - after write to device, poll for status
 *
 * We have a very complex set of IO requirements here.  First, there is
 * the problem of the device and its status return.  If we have a 'real'
 * device,  then it is usually opened RW, so we can get status back.
 * In this case, output == status_device.  If we have a 'filter' as an
 * output device,  then we arrange to have the filter device bidirectional.
 * This means that the filter can/must also read the output from the device.
 *
 * This implies that output filters used as devices must handle their status
 * and error reporting themselves,  and provide as little status back on their
 * STDERR as is possible,  otherwise the pipe will block for them on write.
 *
 * So the following is now done:
 *
 *  a) if you have a 'real device' which is opened RW then you
 *     have output == status_device otherwise you have status_device == -1
 *  b) if you have an output device that requires that you poll it for
 *     status after every write,  then you set 'poll_for_status'
 *     This has the effect of temporarily setting 'status_device = -1'
 *     i.e. - not trying to get status.
 *  c) If you need to use a filter,  then you pass the output device to the
 *     filter.  The filter will then need to decide if it can read from
 *     the device.  After the filter has run and you have 'poll_for_status'
 *     you should check for status from the device.
 ****************************************************************************/

int Print_job( int output, int status_device, struct job *job,
	int timeout, int poll_for_status, char *user_filter )
{
	char *FF_str, *leader_str, *trailer_str, *filter;
	int i, of_stdin, of_stderr, if_error[2],
		of_pid, copy, copies,
		do_banner, n, pid, count, size, fd, tempfd,
		files_printed;

	char msg[SMALLBUFFER];
	char filter_name[8], filter_title[64], msgbuffer[SMALLBUFFER],
		filtermsgbuffer[SMALLBUFFER];
	char *id, *s, *banner_name, *transfername, *openname, *format;
	struct line_list *datafile, files;
	struct stat statb;

	of_pid = -1;
	msgbuffer[0] = 0;
	filtermsgbuffer[0] = 0;
	Errorcode = 0;
	Init_line_list(&files);
	of_stdin = of_stderr = tempfd = fd = -1;
	FF_str = leader_str = trailer_str = 0;
	files_printed = 0;

	DEBUG2( "Print_job: output fd %d", output );
	if(DEBUGL5){
		LOGDEBUG("Print_job: at start open fd's");
		for( i = 0; i < 20; ++i ){
			if( fstat(i,&statb) == 0 ){
				LOGDEBUG("  fd %d (0%o)", i, statb.st_mode&S_IFMT);
			}
		}
	}
	if(DEBUGL2) Dump_job( "Print_job", job );
	id = Find_str_value(&job->info,IDENTIFIER,Value_sep);
	if( id == 0 ) id = Find_str_value(&job->info,TRANSFERNAME,Value_sep);

	DEBUG2("Print_job: OF_Filter_DYN '%s'", OF_Filter_DYN );

	/* clear output buffer */
	Init_buf(&Outbuf, &Outmax, &Outlen );

	FF_str = Fix_str( Form_feed_DYN );
	leader_str = Fix_str( Leader_on_open_DYN );
	trailer_str = Fix_str( Trailer_on_close_DYN );

	/* Leader_on_open_DYN -> output; */
	if( leader_str ) Put_buf_str( leader_str, &Outbuf, &Outmax, &Outlen );

	/* FF_on_open_DYN -> output; */
	if( FF_on_open_DYN ) Put_buf_str( FF_str, &Outbuf, &Outmax, &Outlen );

	/*
	 * if SupressHeader then no banner
	 * if AlwaysBanner then get user name
	 */


	banner_name = Find_str_value(&job->info, BNRNAME, Value_sep );
	if( Always_banner_DYN && banner_name == 0 ){
		/* we are always going to do a banner; get the user name */
		/* need a name to use */
		banner_name = Find_str_value( &job->info,LOGNAME,Value_sep);
		if( banner_name == 0 ) banner_name = "ANONYMOUS";
		Set_str_value(&job->info,BNRNAME,banner_name);
	}
	/* suppress header overrides everything */
	do_banner = (!Suppress_header_DYN && banner_name);

	/* now we have a banner, is it at start or end? */
	DEBUG2("Print_job: do_banner %d, :hl=%d, :bs=%s, :be=%s, banner_name '%s'",
			do_banner, Banner_last_DYN, Banner_start_DYN, Banner_end_DYN, banner_name );
	if( do_banner && !Banner_last_DYN ){
		Print_banner( banner_name, Banner_start_DYN, job );
	}

	DEBUG2("Print_job: setup %d bytes '%s'", Outlen, Outbuf ); 

	msgbuffer[0] = 0;
	/* do we need an OF filter? */
	if( OF_Filter_DYN ){
		if( Run_OF_filter( timeout, &of_pid, &of_stdin, &of_stderr,
			output, &Outbuf, &Outmax, &Outlen,
			job, id, 0,
			msgbuffer, sizeof(msgbuffer)-1 ) ){
			goto exit;
		}
	} else if( Outlen ){
		/* no filter - direct to device */
		n = Write_outbuf_to_OF(job,"LP",output, Outbuf, Outlen,
			status_device, msgbuffer, sizeof(msgbuffer)-1,
			timeout, poll_for_status );
		if( n ){
			Errorcode = JFAIL;
			SETSTATUS(job)"LP device write error '%s'", Server_status(n));
			goto exit;
		}
	}
	Init_buf(&Outbuf, &Outmax, &Outlen );

	/* 
	 *  print out the data files
	 */

	for( count = 0; count < job->datafiles.count; ++count ){
		datafile = (void *)job->datafiles.list[count];
		if(DEBUGL4)Dump_line_list("Print_job - datafile", datafile );

		transfername = Find_str_value(datafile,TRANSFERNAME,Value_sep);
		openname = Find_str_value(datafile,OPENNAME,Value_sep);
		format = Find_str_value(datafile,FORMAT,Value_sep);
		size = Find_flag_value(datafile,SIZE,Value_sep);
		copies = Find_flag_value(datafile,COPIES,Value_sep);
		if( copies == 0 ) copies = 1;

		Set_str_value(&job->info,FORMAT,format);
		Set_str_value(&job->info,DF_NAME,transfername);

		s = Find_str_value(datafile,"N",Value_sep);
		Set_str_value(&job->info,"N",s);

		/*
		 * now we check to see if there is an input filter
		 */
		SNPRINTF(filter_name,sizeof(filter_name))"%s","if");
		filter_name[0] = cval(format);
		filter = user_filter;
		switch( cval(format) ){
			case 'p': case 'f': case 'l':
				filter_name[0] = 'i';
				if( !filter ) filter = IF_Filter_DYN;
				break;
			case 'a': case 'i': case 'o': case 's':
				SETSTATUS(job)"bad data file format '%c', using 'if' filter", cval(format) );
				filter_name[0] = 'i';
				if( !filter ) filter = IF_Filter_DYN;
				break;
		}
		if( !filter ){
			filter = Find_str_value(&PC_entry_line_list,
				filter_name,Value_sep);
		}
		if( !filter){
			filter = Find_str_value(&Config_line_list,filter_name,
				Value_sep);
		}
		if( filter == 0 ) filter = Filter_DYN;
		DEBUG3("Print_job: format '%s', filter '%s'", format, filter );

		uppercase(filter_name);
		if( filter ){
			s = filter;
			if( cval(s) == '(' ){
				++s;
				while( isspace(cval(s))) ++s;
			} else {
				if( !(s = strchr(filter,'/')) ) s = filter;
			}
			SNPRINTF(msg, sizeof(msg)) "%s", s );
			if( (s = strpbrk(msg,Whitespace)) ) *s = 0;
			if( (s = strrchr(msg,'/')) ) memmove(msg,s+1,strlen(s+1)+1);
		} else {
			SNPRINTF(msg, sizeof(msg)) "%s", "none - passthrough" );
		}
		SNPRINTF(filter_title,sizeof(filter_title))"%s filter '%s'",
			filter_name, msg );

		if( fd >= 0 ) close(fd); fd = -1;
		if( !Is_server && openname == 0 ){
			fd = 0;
			DEBUG3("Print_job: taking file from STDIN" );
		} else if( (fd = Checkread( openname, &statb )) < 0 ){
			Errorcode = JFAIL;
			LOGMSG( LOG_ERR) "Print_job: job '%s', cannot open data file '%s'",
				id, openname );
			goto end_of_job;
		}
		SETSTATUS(job)"processing '%s', size %0.0f, format '%s', %s",
			transfername, (double)statb.st_size, format, filter_title );
		if( cval(format) == 'p' ){
			DEBUG3("Print_job: using 'p' formatter '%s'", Pr_program_DYN );
			SETSTATUS(job)"format 'p' pretty printer '%s'", Pr_program_DYN);
			if( Pr_program_DYN == 0 ){
				SETSTATUS(job)"no 'p' format filter available" );
				Errorcode = JABORT;
				goto end_of_job;
			}
			tempfd = Make_temp_fd(0);
			n = Filter_file( fd, tempfd, "PR_PROGRAM",
				Pr_program_DYN, 0, job, 0, 1 );
			if( n ){
				Errorcode = JABORT;
				LOGERR(LOG_INFO)"Print_job:  could not make '%s' process",
					Pr_program_DYN );
				goto end_of_job;
			}
			close(fd); fd = tempfd;
			if( fstat(fd, &statb ) == -1 ){
				Errorcode = JABORT;
				LOGERR(LOG_INFO)"Print_job: fstat() failed");
			}
			SETSTATUS(job)"data file '%s', size now %0.0f",
				transfername, (double)statb.st_size );
		}
		for( copy = 0; copy < copies; ++copy ){
			if( fd && lseek(fd,0,SEEK_SET) == -1 ){
				Errorcode = JABORT;
				LOGERR(LOG_INFO)"Print_job:  lseek tempfd failed");
				goto end_of_job;
			}
			if( copies > 1 ){
				SETSTATUS(job)"doing copy %d of %d", copy+1, copies );
			}
			if(DEBUGL5){
				LOGDEBUG("Print_job: doing '%s' open fd's", openname);
				for( i = 0; i < 20; ++i ) if( fstat(i,&statb) == 0 )
					 LOGDEBUG("  fd %d (0%o)", i, statb.st_mode&S_IFMT);
			}
			Init_buf(&Outbuf, &Outmax, &Outlen );
			if( files_printed++ && !No_FF_separator_DYN && FF_str ){
				/* FF separator -> of_fd; */
				SETSTATUS(job)"printing '%s' FF separator ",id);
				Put_buf_str( FF_str, &Outbuf, &Outmax, &Outlen );
			}
			/* do we have output for the OF device/filter ? */
			if( Outlen > 0 ){
				/* yes */
				if( OF_Filter_DYN ){
					/* send it to the OF filter */
					if( Run_OF_filter( timeout, &of_pid, &of_stdin, &of_stderr,
						output, &Outbuf, &Outmax, &Outlen,
						job, id, 0,
						msgbuffer, sizeof(msgbuffer)-1 ) ){
						goto exit;
					}
				} else {
					/* send it to the OF device */
					n = Write_outbuf_to_OF(job,"LP",output, Outbuf, Outlen,
						status_device, msgbuffer, sizeof(msgbuffer)-1,
						timeout, poll_for_status );
					if( n ){
						Errorcode = n;
						SETSTATUS(job)"error writing to device '%s'",
							Server_status(n));
						goto end_of_job;
					}
				}
				Init_buf(&Outbuf, &Outmax, &Outlen );
			}

			if( filter ){
				DEBUG3("Print_job: format '%s' starting filter '%s'",
					format, filter );
				DEBUG2("Print_job: filter_stderr_to_status_file %d, ps '%s'",
					Filter_stderr_to_status_file_DYN, Status_file_DYN );
				if_error[0] = if_error[1] = -1;
				if( Filter_stderr_to_status_file_DYN && Status_file_DYN && *Status_file_DYN ){
					if_error[1] = Checkwrite( Status_file_DYN, &statb, O_WRONLY|O_APPEND, 0, 0 );
				} else if( pipe( if_error ) == -1 ){
					Errorcode = JFAIL;
					LOGERR(LOG_INFO)"Print_job: pipe() failed");
					goto end_of_job;
				}
				Max_open(if_error[0]); Max_open(if_error[1]);
				DEBUG3("Print_job: %s fd if_error[%d,%d]", filter_title,
					 if_error[0], if_error[1] );
				s = 0;
				if( Backwards_compatible_filter_DYN ) s = BK_filter_options_DYN;
				if( s == 0 ) s = Filter_options_DYN;

				Free_line_list(&files);
				Check_max(&files, 10 );
				files.list[files.count++] = Cast_int_to_voidstar(fd);		/* stdin */
				files.list[files.count++] = Cast_int_to_voidstar(output);	/* stdout */
				files.list[files.count++] = Cast_int_to_voidstar(if_error[1]);	/* stderr */
				if( (pid = Make_passthrough( filter, s, &files, job, 0 )) < 0 ){
					Errorcode = JFAIL;
					LOGERR(LOG_INFO)"Print_job:  could not make %s process",
						filter_title );
					goto end_of_job;
				}
				files.count = 0;
				Free_line_list(&files);

				if( (close(if_error[1]) == -1 ) ){
					Errorcode = JFAIL;
					LOGERR_DIE(LOG_INFO)"Print_job: X5 close(%d) failed",
						if_error[1]);
				}
				if_error[1] = -1;
				Init_buf(&Outbuf, &Outmax, &Outlen );

				filtermsgbuffer[0] = 0;
				if( if_error[0] != -1 ){
					n = Get_status_from_OF(job,filter_title,pid,
						if_error[0], filtermsgbuffer, sizeof(filtermsgbuffer)-1,
						timeout, 0, 0 );
					if( filtermsgbuffer[0] ){
						SETSTATUS(job) "%s filter msg - '%s'", filter_title, filtermsgbuffer );
					}
					if( n ){
						Errorcode = n;
						SETSTATUS(job)"%s filter problems, error '%s'",
							filter_title, Server_status(n));
						goto end_of_job;
					}
					close(if_error[0]);
					if_error[0] = -1;
				}
				/* now we get the exit status for the filter */
				n = Wait_for_pid( pid, filter_title, 0, timeout );
				if( n ){
					Errorcode = n;
					SETSTATUS(job)"%s filter exit status '%s'",
						filter_title, Server_status(n));
					goto end_of_job;
				}
				SETSTATUS(job) "%s filter finished", filter_title );
			} else {
				/* we write to the output device, and then get status */
				DEBUG3("Print_job: format '%s' no filter, reading from %d",
					format, fd );
				Init_buf(&Outbuf, &Outmax, &Outlen );
				while( (Outlen = read(fd,Outbuf,Outmax)) > 0 ){
					Outbuf[Outlen] = 0;
					n = Write_outbuf_to_OF(job,"LP",output, Outbuf, Outlen,
						status_device, msgbuffer, sizeof(msgbuffer)-1,
						timeout, poll_for_status );
					if( n ){
						Errorcode = JFAIL;
						SETSTATUS(job)"error '%s'", Server_status(n));
					}
				}
				if( Outlen < 0 ){
					Errorcode = JFAIL;
					SETSTATUS(job)"error reading file '%s'", Errormsg(errno));
				}
				Outlen = 0;
			}
			DEBUG3("Print_job: finished copy");
		}
		DEBUG3("Print_job: finished file");
	}

	/* 
	 * now we do the end
	 */
 end_of_job:

	DEBUG3("Print_job: end of job");
	Init_buf(&Outbuf, &Outmax, &Outlen );

	/* check for the banner at the end */

	if( do_banner && (Banner_last_DYN || Banner_end_DYN) ){
		Print_banner( banner_name, Banner_end_DYN, job );
	}

	/* 
	 * FF_on_close_DYN     -> of_fd;
	 */ 
	if( FF_on_close_DYN ) Put_buf_str( FF_str, &Outbuf, &Outmax, &Outlen );

	/* 
	 * Trailer_on_close_DYN -> of_fd;
	 */ 
	if( trailer_str ) Put_buf_str( trailer_str, &Outbuf, &Outmax, &Outlen );

	/*
	 * close the OF Filters
	 */

	if( OF_Filter_DYN ){
		if( Run_OF_filter( timeout, &of_pid, &of_stdin, &of_stderr,
			output, &Outbuf, &Outmax, &Outlen,
			job, id, 1,
			msgbuffer, sizeof(msgbuffer)-1 ) ){
			goto exit;
		}
	} else {
		if( Outlen ){
			n = Write_outbuf_to_OF(job,"LP",output, Outbuf, Outlen,
				status_device, msgbuffer, sizeof(msgbuffer)-1,
				timeout, poll_for_status );
			if( n && Errorcode == 0 ){
				Errorcode = JFAIL;
				SETSTATUS(job)"LP device write error '%s'", Errormsg(errno));
				goto exit;
			}
		}
		if( msgbuffer[0] ){
			SETSTATUS(job) "%s filter msg - '%s'", "LP", msgbuffer );
		}
	}
	Init_buf(&Outbuf, &Outmax, &Outlen );
#ifdef HAVE_TCDRAIN
	if( isatty( output ) && tcdrain( output ) == -1 ){
		LOGERR_DIE(LOG_INFO)"Print_job: tcdrain failed");
	}
#endif
	SETSTATUS(job)"printing finished");

 exit:
	Init_buf(&Outbuf, &Outmax, &Outlen );
	if( Outbuf ) free(Outbuf); Outbuf = 0;
	if(FF_str) free(FF_str);
	if(leader_str) free(leader_str);
	if(trailer_str) free(trailer_str);
	if( of_stdin != -1 ) close(of_stdin); of_stdin = -1;
	if( of_stderr != -1 ) close(of_stderr); of_stderr = -1;
	if( tempfd != -1 ) close(tempfd); tempfd = -1;
	if( fd != -1 ) close(fd); fd = -1;
	if(DEBUGL3){
		LOGDEBUG("Print_job: at end open fd's");
		for( i = 0; i < 20; ++i ){
			if( fstat(i,&statb) == 0 ){
				LOGDEBUG("  fd %d (0%o)", i, statb.st_mode&S_IFMT);
			}
		}
	}
	return( Errorcode );
}

/*
 * int Create_OF_filter( int *of_stdin, int *of_stderr )
 *  of_stdin = STDIN of filter (writable)
 *  of_stderr = STDERR of filter (readable)
 *   - we create the OF filter and return the PID
 *  RETURNS:
 *
 */

 static const char *Filter_stop = "\031\001";

int Run_OF_filter( int timeout, int *of_pid, int *of_stdin, int *of_stderr,
	int output, char **outbuf, int *outmax, int *outlen,
	struct job *job, char *id, int terminate_of,
	char *msgbuffer, int msglen )
{
	char msg[SMALLBUFFER];
	char *s;
	int of_error[2], of_fd[2], n;
	struct stat statb;
	struct line_list files;

	if( *of_pid < 0 ){
		Init_line_list(&files);
		of_fd[0] = of_fd[1] = of_error[0] = of_error[1] = -1;
		*of_stdin = *of_stderr = -1;
		if( !(s = strchr( OF_Filter_DYN, '/' )) ) s = OF_Filter_DYN;
		SNPRINTF( msg, sizeof(msg)) "%s", s );
		if( (s = strpbrk( msg, Whitespace )) ) *s = 0;
		if( (s = strrchr( msg, '/')) ){
			memmove( msg, s+1, strlen(s)+1 );
		}
		SETSTATUS(job)"printing '%s' starting OF '%s'", id, msg );
		if( pipe( of_fd ) == -1 ){
			Errorcode = JFAIL;
			LOGERR(LOG_INFO)"Run_OF_filter: pipe() failed");
			goto exit;
		}
		Max_open(of_fd[0]); Max_open(of_fd[1]);
		DEBUG2("Run_OF_filter: errors_to_ps %d, ps '%s'", Filter_stderr_to_status_file_DYN,
			Status_file_DYN );
		of_error[0] = of_error[1] = -1;
		if( Filter_stderr_to_status_file_DYN && Status_file_DYN && *Status_file_DYN ){
			of_error[1] = Checkwrite( Status_file_DYN, &statb, O_WRONLY|O_APPEND, 0, 0 );
		} else if( pipe( of_error ) == -1 ){
			Errorcode = JFAIL;
			LOGERR(LOG_INFO)"Run_OF_filter: pipe() failed");
			goto exit;
		}
		Max_open(of_error[0]); Max_open(of_error[1]);
		DEBUG3("Run_OF_filter: fd of_fd[%d,%d], of_error[%d,%d]",
			of_fd[0], of_fd[1], of_error[0], of_error[1] );

		/* set format */
		Set_str_value(&job->info,FORMAT,"o");
		/* set up file descriptors */

		s = 0;
		if( Backwards_compatible_filter_DYN ) s = BK_of_filter_options_DYN;
		if( s == 0 ) s = OF_filter_options_DYN;
		if( s == 0 ) s = Filter_options_DYN;

		Check_max(&files,10);
		files.list[files.count++] = Cast_int_to_voidstar(of_fd[0]);	/* stdin */
		files.list[files.count++] = Cast_int_to_voidstar(output);	/* stdout */
		files.list[files.count++] = Cast_int_to_voidstar(of_error[1]);	/* stderr */
		if( (*of_pid = Make_passthrough( OF_Filter_DYN, s,&files, job, 0 ))<0){
			Errorcode = JFAIL;
			LOGERR(LOG_INFO)"Run_OF_filter: could not create OF process");
			goto exit;
		}
		files.count = 0;
		Free_line_list(&files);

		DEBUG3("Run_OF_filter: OF pid %d", *of_pid );
		if( of_fd[0] > 0 &&  (close( of_fd[0] ) == -1 ) ){
			Errorcode = JFAIL;
			LOGERR(LOG_INFO)"Run_OF_filter: X0 close(%d) failed", of_fd[0]);
			goto exit;
		}
		of_fd[0] = -1;
		if( of_error[1] > 0 && (close( of_error[1] ) == -1 ) ){
			Errorcode = JFAIL;
			LOGERR(LOG_INFO)"Run_OF_filter: X1 close(%d) failed", of_error[1]);
			goto exit;
		}
		of_error[1] = -1;
		DEBUG3("Run_OF_filter: writing init to OF pid '%d', count %d", *of_pid, *outlen );

		*of_stderr = of_error[0];
		*of_stdin = of_fd[1];
	} else {
		kill( *of_pid, SIGCONT );
	}
	if( Suspend_OF_filter_DYN && !terminate_of ){
		Put_buf_str( Filter_stop, outbuf, outmax, outlen );
		n = Write_outbuf_to_OF(job,"OF",*of_stdin,
			*outbuf, *outlen,
			*of_stderr, msgbuffer, msglen,
			timeout, 0 );
		if( n == 0 ){
			n = Get_status_from_OF(job,"OF",*of_pid,
				*of_stderr, msgbuffer, msglen,
				timeout, 1, Filter_poll_interval_DYN );
		}
		if( n != JSUSP ){
			Errorcode = n;
			SETSTATUS(job)"OF filter problems, error '%s'", Server_status(n));
			goto exit;
		}
		SETSTATUS(job)"OF filter suspended" );
	} else {
		n = Write_outbuf_to_OF(job,"OF",*of_stdin,
			*outbuf, *outlen,
			*of_stderr, msgbuffer, msglen,
			timeout, 0 );
		if( n ){
			Errorcode = n;
			SETSTATUS(job)"OF filter problems, error '%s'", Server_status(n));
			goto exit;
		}
		close( *of_stdin );
		*of_stdin = -1;
		n = Get_status_from_OF(job,"OF",*of_pid,
			*of_stderr, msgbuffer, msglen,
			timeout, 0, 0 );
		if( n ){
			Errorcode = n;
			SETSTATUS(job)"OF filter problems, error '%s'", Server_status(n));
			goto exit;
		}
		close( *of_stderr );
		*of_stdin = -1;
		/* now we get the exit status for the filter */
		n = Wait_for_pid( *of_pid, "OF", 0, timeout );
		if( n ){
			Errorcode = n;
			SETSTATUS(job)"%s filter exit status '%s'",
				"OF", Server_status(n));
			goto exit;
		}
		*of_pid = -1;
		SETSTATUS(job)"OF filter finished" );
	}
	return( 0 );
 exit:
	return( -1 );
}

/*
 * Print a banner
 * check for a small or large banner as necessary
 */

void Print_banner( char *name, char *pgm, struct job *job )
{
	char buffer[LARGEBUFFER];
	int len, n;
	char *bl = 0;
	int tempfd;

	/*
	 * print the banner
	 */
	if(DEBUGL3){
		struct stat statb; int i;
		LOGDEBUG("Print_banner: at start open fd's");
		for( i = 0; i < 20; ++i ){
			if( fstat(i,&statb) == 0 ){
				LOGDEBUG("  fd %d (0%o)", i, statb.st_mode&S_IFMT);
			}
		}
	}
	if( !pgm ) pgm = Banner_printer_DYN;

	DEBUG2( "Print_banner: name '%s', pgm '%s', sb=%d, Banner_line_DYN '%s'",
		name, pgm, Short_banner_DYN, Banner_line_DYN );

	if( !pgm && !Short_banner_DYN ){
		return;
	}

 	if( pgm ){
		/* we now need to create a banner */
		SETSTATUS(job)"creating banner");

		tempfd = Make_temp_fd(0);
		n = Filter_file( -1, tempfd, "BANNER",
			pgm, Filter_options_DYN, job, 0, 1 );
		if( n ){
			Errorcode = JFAIL;
			LOGERR_DIE(LOG_INFO)
			"Print_banner: banner pgr '%s' exit status '%s'",
			pgm, Server_status(n));
		}

		if( lseek(tempfd,0,SEEK_SET) == -1 ){
			Errorcode = JFAIL;
			LOGERR_DIE(LOG_INFO)"Print_banner: fseek(%d) failed", tempfd);
		}
		len = Outlen;
		while( (n = read(tempfd, buffer, sizeof(buffer))) > 0 ){
			Put_buf_len(buffer, n, &Outbuf, &Outmax, &Outlen );
		}
		if( (close(tempfd) == -1 ) ){
			Errorcode = JFAIL;
			LOGERR_DIE(LOG_INFO)"Print_banner: Xa close(%d) failed",
				tempfd);
		}
		DEBUG4("Print_banner: BANNER '%s'", Outbuf+len);
	} else {
		struct line_list l;
		Init_line_list(&l);
		SETSTATUS(job)"inserting short banner line");
		Add_line_list(&l,Banner_line_DYN,0,0,0);
		Fix_dollars(&l,job,1,Filter_options_DYN);
		bl = safestrdup2(l.list[0],"\n",__FILE__,__LINE__);
		Put_buf_str( bl, &Outbuf, &Outmax, &Outlen );
		Free_line_list(&l);
		if( bl ) free(bl); bl = 0;
	}
	if(DEBUGL3){
		struct stat statb; int i;
		LOGDEBUG("Print_banner: at end open fd's");
		for( i = 0; i < 20; ++i ){
			if( fstat(i,&statb) == 0 ){
				LOGDEBUG("  fd %d (0%o)", i, statb.st_mode&S_IFMT);
			}
		}
	}
}

/*
 * Write_outbuf_to_OF(
 * int of_fd, buffer, outlen     - write to this
 * int of_error  - read status from this
 * char *msg, int msgmax - status storage area
 * int timeout   - timeout
 *                 nnn - wait this long
 *                 0   - wait indefinately
 *                 -1  - do not wait
 * poll for status
 * )
 * We write the output buffer to the OF process, and then wait for it to
 * either exit or suspend itself.
 *     JSUCC    = 0
 *     JTIMEOUT  - timeout
 *     JWRERR     -  (-1 originally) - error reading or writing
 *     JRDERR     -  (-1 originally) - error reading or writing
 */

int Write_outbuf_to_OF( struct job *job, char *title,
	int of_fd, char *buffer, int outlen,
	int of_error, char *msg, int msgmax,
	int timeout, int poll_for_status )
{
	time_t start_time, current_time;
	int msglen, return_status, count, elapsed, left;
	struct stat statb;
	char *s;

	DEBUG3(
		"Write_outbuf_to_OF: len %d, of_fd %d, of_error %d, timeout %d, poll_for_status %d",
		outlen, of_fd, of_error, timeout, poll_for_status );

	start_time = time((void *)0);
	return_status = 0;
	if( outlen == 0 ) return return_status;
	if( of_fd >= 0 && fstat( of_fd, &statb ) ){
		Errorcode = JABORT;
		LOGERR_DIE(LOG_INFO) "Write_outbuf_to_OF: %s, of_fd %d closed!",
		title, of_fd );
	}
	if( of_error > 0 && fstat( of_error, &statb ) ){
		LOGERR(LOG_INFO) "Write_outbuf_to_OF: %s, of_error %d closed!",
			title, of_error );
		of_error = -1;
	}
	if( of_error < 0 ){
		return_status = Write_fd_len_timeout( timeout, of_fd, buffer, outlen );
		DEBUG4("Write_outbuf_to_OF: Write_fd_len_timeout result %d", return_status );
	} else if( poll_for_status ){
		return_status = Write_fd_len_timeout( timeout, of_fd, buffer, outlen );
		DEBUG4("Write_outbuf_to_OF: Write_fd_len_timeout result %d", return_status );
		do {
			msglen = strlen(msg);
			if( msglen >= msgmax ){
				SETSTATUS(job) "%s filter msg - '%s'", title, msg );
				msg[0] = 0;
				msglen = 0;
			}
			count = -1;
			/* we put a 1 second timeout here, just to make sure */
			count = Read_fd_len_timeout( 1, of_error, msg+msglen, msgmax-msglen );
			if( count > 0 ){
				msglen += count;
				msg[msglen] = 0;
				while( (s = safestrchr(msg,'\n')) ){
					*s++ = 0;
					SETSTATUS(job) "%s filter msg - '%s'", title, msg );
					memmove(msg,s,strlen(s)+1);
				}
			}
		} while( count > 0 );
	} else while( return_status == 0 && outlen > 0 ){
		left = timeout;
		if( timeout > 0 ){
			current_time = time((void *)0);
			elapsed = current_time - start_time;
			left = timeout - elapsed;
			if( left <= 0 ){
				return_status = JTIMEOUT;
				break;
			}
		}
		msglen = strlen(msg);
		if( msglen >= msgmax ){
			SETSTATUS(job) "%s filter msg - '%s'", title, msg );
			msg[0] = 0;
			msglen = 0;
		}
		count = -1;	/* number read into msg buffer */
		DEBUG4("Write_outbuf_to_OF: writing %d", outlen );
		return_status = Read_write_timeout( of_error, msg+msglen, msgmax-msglen, &count,
			of_fd, &buffer, &outlen, left );
		DEBUG4("Write_outbuf_to_OF: return_status %d, count %d, '%s'",
			return_status, count, msg);
		if( DEBUGL4 ){
			char smb[32]; SNPRINTF(smb,sizeof(smb))"%s",msg);
			logDebug("Write_outbuf_to_OF: writing '%s...'", smb );
		}
		if( count > 0 ){
			msglen += count;
			msg[msglen] = 0;
			s = msg;
			while( (s = safestrchr(msg,'\n')) ){
				*s++ = 0;
				SETSTATUS(job) "%s filter msg - '%s'", title, msg );
				memmove(msg,s,strlen(s)+1);
			}
		}
	}
	if( return_status < 0 ) return_status = JWRERR;
	DEBUG3("Write_outbuf_to_OF: after write return_status %d, of_fd %d, of_error %d",
		return_status, of_fd, of_error );
	/* read and see if there is any status coming back */
	return( return_status );
}

/*
 * int Get_status_from_OF( struct job *job, char *title, int of_pid,
 *    int of_error, char *msg, int msgmax,
 *	  int timeout, int suspend, int max_wait )
 * return:
 *   0 successful
 *   JTIMEOUT - timeout
 */

int Get_status_from_OF( struct job *job, char *title, int of_pid,
	int of_error, char *msg, int msgmax,
	int timeout, int suspend, int max_wait )
{
	time_t start_time, current_time;
	int m, msglen, return_status, count, elapsed, left, done;
	struct stat statb;
	char *s;

	start_time = time((void *)0);
	DEBUG3( "Get_status_from_OF: pid %d, of_error %d, timeout %d",
		of_pid, of_error, timeout );

	return_status = 0;

	if( fstat( of_error, &statb ) ){
		Errorcode = JABORT;
		LOGERR_DIE(LOG_INFO) "Get_status_from_OF: %s, of_error %d closed!",
			title, of_error );
	}

	done = 0;
	while( !done ){
		left = timeout;
		if( timeout > 0 ){
			current_time = time((void *)0);
			elapsed = current_time - start_time;
			left = timeout - elapsed;
			if( left <= 0 ){
				return_status = JTIMEOUT;
				break;
			}
		}
		DEBUG3("Get_status_from_OF: waiting for '%s', left %d secs for pid %d",
			suspend?"suspend":"exit", left, of_pid );
		count = -1;
		m = 0;
		/* we see if we have output */
		if( suspend ){
			/* poll for process suspend status */
			left = max_wait>0?max_wait:1;
			DEBUG3("Get_status_from_OF: polling interval %d", left );
			return_status = Wait_for_pid( of_pid, title, suspend, left );
			DEBUG4("Get_status_from_OF: return_status '%s'", Server_status(return_status));
			/* we do a poll, just to see if the process is blocked because the
			 * pipe is full.  We may need to read the pipe to clear out the buffer
			 * so it can exit.  This is really an unusual condition,  but it can happen.
			 */
			left = -1;
			if( return_status == JTIMEOUT ){
				/* had a timeout */
				return_status = 0;
			} else {
				done = 1;
			}
			DEBUG4("Get_status_from_OF: now reading, after suspend" );
			do{
				msglen = strlen(msg);
				if( msglen >= msgmax ){
					SETSTATUS(job) "%s filter msg - '%s'", title, msg );
					msg[0] = 0;
					msglen = 0;
				}
				count = -1;
				Set_nonblock_io( of_error );
				count = read( of_error, msg+msglen, msgmax-msglen );
				Set_block_io( of_error );
				if( count > 0 ){
					while( (s = safestrchr(msg,'\n')) ){
						*s++ = 0;
						SETSTATUS(job) "%s filter msg - '%s'", title, msg );
						memmove(msg,s,strlen(s)+1);
					}
				}
			} while( count > 0 );
		} else do {
			/* now we read the error output, just in case there is something there */
			DEBUG4("Get_status_from_OF: now reading, left %d", left );
			msglen = strlen(msg);
			if( msglen >= msgmax ){
				SETSTATUS(job) "%s filter msg - '%s'", title, msg );
				msg[0] = 0;
				msglen = 0;
			}
			count = Read_fd_len_timeout( left, of_error, msg+msglen, msgmax-msglen );
			if( count > 0 ){
				msglen += count;
				msg[msglen] = 0;
				s = msg;
				while( (s = safestrchr(msg,'\n')) ){
					*s++ = 0;
					SETSTATUS(job) "%s filter msg - '%s'", title, msg );
					memmove(msg,s,strlen(s)+1);
				}
			} else if( count == 0 ){
				done = 1;
			} else {
				return_status = JTIMEOUT;
				done = 1;
			}
		} while( count > 0 );
	}
	return(return_status);
}

/****************************************************************************
 * int Wait_for_pid( int of_pid, char *name, int suspend, int timeout )
 * of_pid     = pid of the process
 * name       = name for messages
 * suspend    = 1 if you want to wait for suspend, now exit
 * timeout    = length of time to wait - 0 is infinite, -1 is none
 *
 * returns:
 *  JSUCC = 0  - successful error code exit
 *  JSUSP      - successful suspend
 *  JSIGNAL    - signal exit code
 *    >0       - exit code
 *  JTIMEOUT   - EINTR error, probably a timeout
 *  JCHILD     - ECHILD error
 *  JNOWAIT    - nonblocking check, no status
 *
 ****************************************************************************/

int Wait_for_pid( int of_pid, char *name, int suspend, int timeout )
{
	int pid, err, return_code;
 	plp_status_t ps_status;

	DEBUG2("Wait_for_pid: name '%s', pid %d, suspend %d, timeout %d",
		name, of_pid, suspend, timeout );
	errno = 0;
	memset(&ps_status,0,sizeof(ps_status));
	if( timeout > 0 ){
		Set_timeout_break( timeout );
		pid = plp_waitpid(of_pid,&ps_status,suspend?WUNTRACED:0 );
		err = errno;
		Clear_timeout();
	} else if( timeout == 0 ){
		pid = plp_waitpid(of_pid,&ps_status,suspend?WUNTRACED:0);
		err = errno;
	} else {
		pid = plp_waitpid(of_pid,&ps_status,(suspend?WUNTRACED:0)|WNOHANG);
		err = errno;
	}
	DEBUG2("Wait_for_pid: pid %d exit status '%s'",
		pid, Decode_status(&ps_status));
	return_code = 0;
	if( pid > 0 ){
		if( WIFSTOPPED(ps_status) ){
			return_code = JSUSP;
			DEBUG1("Wait_for_pid: %s filter suspended", name );
		} else if( WIFEXITED(ps_status) ){
			return_code = WEXITSTATUS(ps_status);
			if( return_code > 0 && return_code < 32 ) return_code += JFAIL-1;
			DEBUG3( "Wait_for_pid: %s filter exited with status %d",
				name, return_code);
		} else if( WIFSIGNALED(ps_status) ){
			int n;
			n = WTERMSIG(ps_status);
			LOGMSG(LOG_INFO)
				"Wait_for_pid: %s filter died with signal '%s'",name,
				Sigstr(n));
			return_code = JSIGNAL;
		} else {
			return_code = JABORT;
			LOGMSG(LOG_INFO)
				"Wait_for_pid: %s filter did strange things",name);
		}
	} else if( pid < 0 ){
		/* you got an error, and it was ECHILD or EINTR
		 * if it was EINTR, you want to know 
		 */
		if( err == EINTR ) return_code = JTIMEOUT;
		else return_code = JCHILD;
	} else {
		return_code = JNOWAIT;
	}
	DEBUG1("Wait_for_pid: returning '%s', exit status '%s'",
		Server_status(return_code), Decode_status(&ps_status) );
	errno = err;
	return( return_code );
}
