# Generated automatically from Makefile.in by configure.
###########################################################################
# LPRng - An Extended Print Spooler System
#
# Copyright 1988-1995 Patrick Powell, San Diego State University
#     papowell@sdsu.edu
# See LICENSE for conditions of use.
#
###########################################################################
# MODULE: Makefile.in
# PURPOSE: top level Makefile for LPD Software
# $Id: Makefile,v 3.0 1996/05/19 04:05:23 papowell Exp $
########################################################################## 

#**************###########**************
# You must use ANSI C and GNU Make!
#***************************************

SRC=.


#=============================================================================
# List the directories you want to generate:
# DIRS for all, clean, etc.
# ALLDIRS for other such as documentation
#=============================================================================

DIRS= src man
ALLDIRS= $(DIRS) ${SRC}/TESTSUPPORT ${SRC}/UTILS ${SRC}/DOC

# define default target
.PHONY: all warn \
	TAGS clean uninstall install \
	info dvi check \
	realclean mostlyclean distclean \
	dist ci cifiles FRC default
MAKETARGET=all

all: warn $(DIRS)

#### USE GNU MAKE #####
warn:
ifdef notdef
	@if ${MAKE} -v 2>&1 | grep -s GNU >/dev/null ; then \
		: ; \
	else \
		echo "Not using GNU make"; \
		exit 1; \
	fi;
endif

#force phony target to be made
$(ALLDIRS): FRC
	$(MAKE) -C $@ $(MAKETARGET) 

#install default versions of the lpd.conf and lpd.perm files
default:
	cp ${SRC}/lpd.conf ${SRC}/lpd.perms /etc

###############################################################################

TAGS clean uninstall install: 
	$(MAKE) MAKETARGET=$@ $(DIRS)

info dvi check:

realclean mostlyclean distclean: clean
	$(MAKE) MAKETARGET=$@ $(ALLDIRS)
	-rm -f config.cache config.h config.log config.status Makefile



###############################################################################

ci: cifiles
	$(MAKE) MAKETARGET=$@ $(ALLDIRS)

cifiles:
	checkin() { \
		ci $(CI) -l -mUpdate -t-Initial $$1; \
	}; \
	for i in *; do \
		if test -f "$$i" ; then \
			case "$$i" in  \
			config.h.in ) checkin $$i;; \
			config.* ) ;; \
			* ) checkin $$i ;; \
			esac; \
		fi; \
	done;

cifast:
	find . -type f -newer VERSION -print \
		| sed \
			-e '/core$$/d' \
			-e '/RCS/d' \
			-e '/\.o$$/d'  \
			-e '/.*liblpr.a$$/d' \
			-e '/.*checkpc$$/d' \
			-e '/.*lpr$$/d' \
			-e '/.*lpd$$/d' \
			-e '/.*lpq$$/d' \
			-e '/.*lprm$$/d' \
			-e '/.*lpc$$/d' \
			-e '/.*lpbanner$$/d' \
			-e '/.*lpf$$/d' \
			-e '/.*lpraccnt$$/d' \
		 >/tmp/list
	cat /tmp/list
	ci $(CI) -l -mUpdate -t-Initial `cat /tmp/list`; \
	

###############################################################################
# Update the patch level when you make a new version
# do this before you start changes
# Don't even think about making this configurable, it is for
# distribution and update purposes only!
#  Patrick Powell
###############################################################################

update: VERSION ./src/include/patchlevel.h 
	$(MAKE) -C man update

VERSION: FRC
	DIR=`pwd | sed 's,.*/,,' `; \
		echo $$DIR >$@ ;
	sleep 1;
	
./src/include/patchlevel.h: FRC
	DIR=`pwd | sed 's,.*/,,' `; \
		echo "#define PATCHLEVEL " \"$$DIR\" >$@ ;

###############################################################################
# Make a gnutar distribution
#   - note that not all the source tree is sent out
#
###############################################################################

shar:
	DIR=`pwd | sed 's,.*/,,' `; \
	cd ..; \
	ls -l $${DIR}.tgz ; \
	if [ ! -f $${DIR}.tgz ]; then \
		echo You must make TAR file first; \
		exit 1; \
	fi; \
	tar ztf $${DIR}.tgz | sed -e '/\/$$/d' | sort >/tmp/_a.list; \
	head /tmp/_a.list; \
	shar -S -n $${DIR} -a -s papowell@sdsu.edu \
	   -c -o /tmp/$${DIR}-Part -l100 </tmp/_a.list; \
	cat $${DIR}/README /tmp/$${DIR}-Part.01 >/tmp/_a.list; \
	cat >/tmp/$${DIR}-Part.01 /tmp/_a.list; \
	rm /tmp/_a.list

dist:
	echo RCS >/tmp/X
	if [ -n "$$NO" ]; then \
		for i in $$NO ; do \
			echo "*/$$i"  >>/tmp/X; \
		done; \
	fi;
	echo src/Makefile >>/tmp/X
	echo man/Makefile >>/tmp/X
	echo TESTSUPPORT/Makefile >>/tmp/X
	echo core >>/tmp/X
	echo '?' >>/tmp/X
	echo '*.o' >>/tmp/X
	echo '*.a' >>/tmp/X
	for i in tags liblpr.a checkpc lpr lpd lpq lprm lpc lpbanner lpf lpraccnt; do \
		echo "*/$$i"  >>/tmp/X; \
	done;
	echo config.cache >>/tmp/X
	echo config.status >>/tmp/X
	echo config.log >>/tmp/X
	echo config.h >>/tmp/X
	cat /tmp/X
	DIR=`pwd | sed 's,.*/,,' `; \
		cd ..; tar zXcfv /tmp/X $${DIR}.tgz $${DIR}
