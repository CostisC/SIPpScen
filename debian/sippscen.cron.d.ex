#
# Regular cron jobs for the sippscen package.
#
0 4	* * *	root	[ -x /usr/bin/sippscen_maintenance ] && /usr/bin/sippscen_maintenance
