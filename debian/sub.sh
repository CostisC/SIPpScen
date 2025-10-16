#!/bin/bash
#
#

for i in SIPpScen*; do 
	newname=`sed 's/SIPpScen\(.*\)/sippscen\1/' <<< $i`
	mv $i $newname
done
