#!/usr/bin/env sh

set -e
#set -x

SOPTS=""
PORT=9292
PROTO="http"

while [ $# -gt 0 ]; do
  case $1 in
    --valgrind)
      VALGRIND=1
      shift
      ;;
    --port)
      shift
      PORT="$1"
      SOPTS="${SOPTS} --port ${PORT}"
      shift
      ;;
    --)      
      shift
      SOPTS="${SOPTS} $@"
      break
      ;;
    *)
      shift
      ;;
  esac
done

if [ -z "${PORT}" ]; then
  echo "Invalid --port specified"
  exit 1
fi

if echo "${SOPTS}" | grep '\-\-ssl'; then
  PROTO="https"
fi

SERVER="./server2 ${SOPTS}"

run() {
  sleep 1

  BASE="${PROTO}://localhost:${PORT}"
  FILTER='sed -r /(date|user-agent|trying|tcp)|^\*/Id'

  echo "\n\nGet empty:"
  curl -isk ${BASE}/get/empty | ${FILTER}

  echo "\n\nGet not-found:"
  curl -isk ${BASE}/get/not_found | ${FILTER}

  echo "\n\nGet query:"
  curl -isk "${BASE}/get/query?foo=bar&baz=a%40z" | ${FILTER}
  
  echo "\n\nGet fail:"
  curl -vsk "${BASE}/fail" 2>&1 | grep -i empty

  echo "\n\nPost urlencoded:"
  curl -isk -d'foo=bar&baz=a%40z' ${BASE}/post/urlencoded | ${FILTER}

  echo "\n\nPut data:"
  curl -isk -XPUT -H'Content-Type:text/plain' -d'ff5fd857-c90b-4066-910f-a9a5d1fa1b47' ${BASE}/post/putdata | ${FILTER}

  base64 /dev/urandom | head -c 25165824 > ./test.dat

  echo "\n\nPost chunked:"
  curl -sk -XPOST -H'Expect:' -H'Transfer-Encoding: chunked' --data-urlencode bigparam@test.dat -o r1.dat \
    ${BASE}/post/bigparam 

  diff ./test.dat ./r1.dat 
  
  echo "\n\nPost multipart:"
  curl -sk -XPOST -H'Expect:' -F 'foo=bar' -F 'baz=a%40z' -F 'bigparam=@test.dat;type=text/plain' \
    ${BASE}/post/multipart | ${FILTER}
}

if [ -n "${VALGRIND}" ]; then
  SERVER="valgrind --leak-check=full --log-file=valgrind2.log ${SERVER}"
fi

echo "Command: ${SERVER}"
${SERVER} &
SPID="$!"

echo "HTTP Server pid: $!"

run 2>&1 | tee server2.log
kill -2 ${SPID}

diff --strip-trailing-cr server2.log server2-success.log
wait ${SPID}

if [ -n "${VALGRIND}" ]; then
  cat ./valgrind2.log | cut -d = -f 5 | grep ERROR > valgrind2-results.log 
  cat ./valgrind2.log | cut -d = -f 5 | grep 'All heap blocks were freed' >> valgrind2-results.log 
  diff valgrind2-results.log valgrind2-success.log
fi  

echo "\nDone!"


