#!/bin/bash

HERE=$(realpath $(dirname $0))
SERVER="$SERVER_B" CLIENT="$CLIENT_B" "${HERE}/__download.sh" "$@"