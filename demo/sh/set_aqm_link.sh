#!/bin/bash
if [ "$#" != "3" ]; then
        echo "usage: ./set_aqm.sh <aqm> <link> <brtt>"
	exit 65
fi

HERE=$(realpath $(dirname $0))

aqm=$1
rate=$2
brtt=$3

do_tc() {
	local cmd="sudo ${HERE}/../../iproute2-l4s/tc/tc $1"
	echo $cmd
	$cmd
}

__set_aqm() {
	aqmname2=$1
	aqmpars="$2"

	echo "# setting $aqmname2 $aqmpars"
	do_tc "qdisc del dev $IFACE root"
	do_tc "qdisc add dev $IFACE root handle 1: htb default 5"
	do_tc "class add dev $IFACE parent 1: classid 1:5 htb rate 1Mbit"
	do_tc "class add dev $IFACE parent 1: classid 1:10 htb rate ${rate}Mbit ceil ${rate}Mbit burst 1514 cburst 1514"
	do_tc "filter add dev $IFACE protocol ip parent 1:0 prio 1 u32 match ip src ${SRC_NET} flowid 1:10"

	#set extra rtt
    do_tc "qdisc del dev ${REV_IFACE} root"
    do_tc "qdisc add dev ${REV_IFACE} root netem delay ${brtt}.0ms limit 40000"

	do_tc "qdisc add dev $IFACE parent 1:10 $aqmname2 $aqmpars"

	do_tc "qdisc show"
}

set_taildrop() {
    local delay=$1
    # the limit is in bytes
    __set_aqm taildrop "limit $((rate * 1000000 * delay / 1000 / 8))b"
}

set_taildrop_pkt() {
    local cnt=$1
    __set_aqm taildrop "limit $((cnt * 1514))b"
}

set_aqm() {
    __set_aqm "$1" "$2 limit 40000"
}

if [[ $aqm == "dpi2" ]]; then
		set_aqm dualpi2 ""  
elif [[ $aqm == "dpi2_dc" ]]; then
        set_aqm dualpi2 "any_ect"  
elif [[ $aqm == "dpi2_dev" ]]; then
		set_aqm dualpi2_dev ""  
elif [[ $aqm == "dpi2_dev_dc" ]]; then
        set_aqm dualpi2_dev "dc_dualq dc_ecn"  
elif [[ $aqm == "td_10" ]]; then
        set_taildrop 10  
elif [[ $aqm == "td_20" ]]; then
        set_taildrop 20  
elif [[ $aqm == "td_50" ]]; then
        set_taildrop 50  
elif [[ $aqm == "td_100" ]]; then
        set_taildrop 100  
elif [[ $aqm == "td_200" ]]; then
        set_taildrop 200  
elif [[ $aqm == "td_5pkt" ]]; then
        set_taildrop_pkt 5
elif [[ $aqm == "td_10pkt" ]]; then
        set_taildrop_pkt 10
elif [[ $aqm == "td_20pkt" ]]; then
        set_taildrop_pkt 20
elif [[ $aqm == "td_100pkt" ]]; then
        set_taildrop_pkt 100
elif [[ $aqm == "td_1000pkt" ]]; then
        set_taildrop_pkt 1000
elif [[ $aqm == "pie" ]]; then
        set_aqm pie "ecn"  
elif [[ $aqm == "pie_40" ]]; then
        set_aqm pie "target 40ms ecn"  
elif [[ $aqm == "pie_10" ]]; then
        set_aqm pie "target 10ms ecn"  
elif [[ $aqm == "pie_5" ]]; then
        set_aqm pie "target 5ms ecn"  
elif [[ $aqm == "pie_1" ]]; then
        set_aqm pie "target 1ms ecn"  
elif [[ $aqm == "fqcodel" ]]; then
        set_aqm fq_codel_tst "ecn"  
elif [[ $aqm == "fqcodel_ce1" ]]; then
        set_aqm fq_codel_tst "ecn ce_threshold 1ms"  
elif [[ $aqm == "fqcodel_ce2" ]]; then
        set_aqm fq_codel_tst "ecn ce_threshold 2ms"  
elif [[ $aqm == "red" ]]; then
        set_aqm red "limit 1600000 min 120000 max 360000 avpkt 1000 burst 220 ecn bandwidth ${link}Mbit"  
fi
