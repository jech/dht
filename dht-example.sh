#!/bin/bash

# Determine path of script
BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Pick random port number from range 1024-65535
port=0
while (( port < 1024 || port > 65535 )); do
    port=$(($RANDOM + $RANDOM))
done
echo "Choosing random port '${port}'"

# Lookup well-known bootstrap nodes and add results to command line
BSNODES=(
    router.bittorrent.com:6881
    router.utorrent.com:6881
    dht.transmissionbt.com:6881
    dht.aelitis.com:6881
    dht.libtorrent.org:25401
    #router.bitcomet.com:6881   # does not seem to be active anymore
    #router.bitcomet.net:554    # resolves to localhost (?)
)
nodeargs=()
for ((i=0; i < ${#BSNODES[@]}; ++i)); do
    echo "Loooking up bootstrap node '${BSNODES[i]}':"
    readarray -d ":" -t hostinfo <<< "${BSNODES[i]}"
    while read -r line; do
        if [[ "${line}" =~ ^([0-9a-f.:]+)[[:space:]]+DGRAM$ ]]; then
            echo "${line}"
            nodeargs+=("${BASH_REMATCH[1]}" "${hostinfo[1]}")
        fi
    done < <(getent ahosts "${hostinfo[0]}")
done

# Run example
echo "Running dht-example:"
echo "# dht-example" ${port} ${nodeargs[@]}
"${BASEDIR}"/dht-example ${port} ${nodeargs[@]}
