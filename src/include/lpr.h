/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-2001, Patrick Powell, San Diego, CA
 *     papowell@lprng.com
 * See LICENSE for conditions of use.
 * $Id: lpr.h,v 1.19 2001/09/18 01:43:46 papowell Exp $
 ***************************************************************************/



#ifndef _LPR_1_
#define _LPR_1_

EXTERN char *Accntname_JOB; /* Accounting name: PLP 'R' control file option */
EXTERN int Auth_JOB;        /* Use authentication */
EXTERN int Binary_JOB;      /* Binary format: 'l' Format */
EXTERN char *Bnrname_JOB;   /* Banner name: RFC 'L' option */
EXTERN char *Classname_JOB; /* Class name:  RFC 'C' option */
EXTERN int Copies_JOB;      /* Copies */
EXTERN int Direct_JOB;      /* Connect and send to TCP/IP port */
EXTERN int Format_JOB;      /* format for printing: lower case letter */
EXTERN char *Font1_JOB;     /* Font information 1 */
EXTERN char *Font2_JOB;     /* Font information 2 */
EXTERN char *Font3_JOB;     /* Font information 3 */
EXTERN char *Font4_JOB;     /* Font information 4 */
EXTERN int Indent_JOB;      /* indent:      RFC 'I' option */
EXTERN char *Jobname_JOB;   /* Job name:    RFC 'J' option */
EXTERN int Lpr_zero_file_JOB;  /* LPR does file filtering and job flattening */
EXTERN int Lpr_bounce_JOB;  /* LPR does file filtering and job flattening */
EXTERN char *Mailname_JOB;  /* Mail name:   RFC 'M' option */
EXTERN int No_header_JOB;   /* No header flag: no L option in control file */
EXTERN int Priority_JOB;	/* Priority */
EXTERN char *Printer_JOB;		/* Printer passed as option */
EXTERN char *Prtitle_JOB;   /* Pr title:    RFC 'T' option */
EXTERN int Pwidth_JOB;	    /* Width paper: RFC 'W' option */
EXTERN int Removefiles_JOB;	    /* Remove files */
EXTERN char *Username_JOB;	/* Specified with the -U option */
EXTERN char *Zopts_JOB;     /* Z options */
EXTERN char * User_filter_JOB; /* User specified filter for job files */

extern struct jobwords Lpr_parms[]; /* parameters for LPR */
EXTERN int LP_mode_JOB;		/* look like LP */
EXTERN int Silent_JOB;			/* lp -s option */
EXTERN int Job_number;

/* PROTOTYPES */
int main(int argc, char *argv[], char *envp[]);
void Get_parms(int argc, char *argv[] );
void usage(void);
int Make_job( struct job *job );
void get_job_number( struct job *job );
double Copy_stdin( struct job *job );
double Check_files( struct job *job );
int Check_lpr_printable(char *file, int fd, struct stat *statb, int format );
int is_exec( char *buf, int n);
int is_arch(char *buf, int n);
void Dienoarg(int option);
void Check_int_dup (int option, int *value, char *arg, int maxvalue);
void Check_str_dup(int option, char **value, char *arg, int maxlen );
void Check_dup(int option, int *value);
int Start_worker( struct line_list *l, int fd );

#endif