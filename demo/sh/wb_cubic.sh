#!/bin/bash

RENOSERVER=$SERVER_B
RENOCLIENT=$CLIENT_B

RENOPORT="11001"
TGEN="traffic_generator"

declare -i mit_r=$1*1000
link=$2

ssh $RENOSERVER 'killall run_httpserver'
if (( "$mit_r" != "0" )); then
	ssh $RENOSERVER "${TGEN}/http_server/run_httpserver 10002 ${TGEN}/gen_rsample/rs2.txt" &
fi
ssh $RENOCLIENT 'killall http_clients_itime'
if (( "$mit_r" != "0" )); then
	ssh $RENOCLIENT "${TGEN}/http_client/http_clients_itime ${RENOSERVER} 10002 ${TGEN}/gen_ritime/rit${mit_r}_2.txt $link ${AQMNODE} ${RENOPORT}" &
fi

