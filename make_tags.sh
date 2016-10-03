#!/bin/bash
TAGS="tags"
if [ "$1" == "emacs" ];then
	TAGS="TAGS"
fi

echo "make ${TAGS} ARCH=arm"
make ${TAGS} ARCH=arm &
TAGS_PID=$!

echo "make cscope ARCH=arm"
make cscope ARCH=arm &
CS_PID=$!

wait ${TAGS_PID} ${CS_PID}
