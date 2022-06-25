#!/bin/bash
iheaders "$@"
rm iheaders-dummy.h
touch iheaders-dummy.h
echo -n "/* " >> iheaders-dummy.h
date >> iheaders-dummy.h
echo -n " */" >> iheaders-dummy.h
