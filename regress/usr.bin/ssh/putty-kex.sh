#	$OpenBSD: putty-kex.sh,v 1.3 2013/05/17 04:29:14 dtucker Exp $
#	Placed in the Public Domain.

tid="putty KEX"

if test "x$REGRESS_INTEROP_PUTTY" != "xyes" ; then
	fatal "putty interop tests not enabled"
fi

for k in dh-gex-sha1 dh-group1-sha1 dh-group14-sha1 ; do
	verbose "$tid: kex $k"
	cp ${OBJ}/.putty/sessions/localhost_proxy \
	    ${OBJ}/.putty/sessions/kex_$k
	echo "KEX=$k" >> ${OBJ}/.putty/sessions/kex_$k

	env HOME=$PWD ${PLINK} -load kex_$k -batch -i putty.rsa2 \
	    127.0.0.1 true
	if [ $? -ne 0 ]; then
		fail "KEX $k failed"
	fi
done

