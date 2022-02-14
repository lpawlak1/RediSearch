#!/bin/bash

[[ $IGNERR == 1 ]] || set -e
# [[ $VERBOSE == 1 ]] && set -x

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT=$(cd $HERE/../.. && pwd)
READIES=$ROOT/deps/readies
. $READIES/shibumi/defs

cd $HERE

#----------------------------------------------------------------------------------------------

if [[ $1 == --help || $1 == help || $HELP == 1 ]]; then
	cat <<-END
		Run Python tests using RLTest

		[ARGVARS...] runtests.sh [--help|help] [<module-so-path>] [extra RLTest args...]

		Argument variables:
		MODULE=path           Path to RediSearch module .so
		BINROOT=path          Path to repo binary root dir

		RLTEST_ARGS=args      Extra RLTest args
		MODARGS=args          RediSearch module arguments
		TEST=name             Operate in single-test mode
		ONLY_STABLE=1         Skip unstable tests

		COORD=1|oss|rlec      Test Coordinator
		SHARDS=n              Number of OSS coordinator shards (default: 3)
		QUICK=1               Perform only one test variant
		PARALLEL=1            Runs RLTest tests in parallel

		REDIS_SERVER=path     Redis Server command
		REDIS_VERBOSE=1       (legacy) Verbose ouput
		CONFIG_FILE=file      Path to config file
		EXT|EXISTING_ENV=1    Run the tests on existing env
		RLEC_PORT=n           Port of RLEC database (default: 12000)

		COV=1				  Run with coverage analysis
		VG=1                  Use valgrind
		VG_LEAKS=0            Do not detect leaks
		SAN=type              Use LLVM sanitizer (type=address|memory|leak|thread) 
		GDB=1                 Enable interactive gdb debugging (in single-test mode)

		VERBOSE=1             Print commands and Redis output
		IGNERR=1              Do not abort on error
		NOP=1                 Dry run
		HELP=1                Show help


	END
	exit 0
fi

#---------------------------------------------------------------------------------------------- 

# If current directory has RLTest in it, then we're good to go
if [[ -d $HERE/RLTest ]]; then
    true
    # Do nothing - tests are already loaded
fi

if [[ -n $RLTEST ]]; then
    # Specifically search for it in the specified location
    PYTHONPATH="$PYTHONPATH:$RLTEST"
else
    # Assume there is a sibling directory called `RLTest`
    PYTHONPATH="$PYTHONPATH:$HERE"
fi

export PYTHONPATH

#---------------------------------------------------------------------------------------------- 

MODULE="${MODULE:-$1}"
shift

[[ $EXT == 1 ]] && EXISTING_ENV=1
[[ $COORD == 1 ]] && COORD=oss

if [[ -z $MODULE ]]; then
	if [[ -n $BINROOT ]]; then
		if [[ -z $COORD ]]; then
			MODULE=$BINROOT/search/redisearch.so
		elif [[ $COORD == oss ]]; then
			MODULE=$BINROOT/oss-coord/module-oss.so
		fi
	fi
fi

OP=
[[ $NOP == 1 ]] && OP=echo

RLTEST_ARGS+=" $@"
if [[ -n $TEST ]]; then
	[[ $GDB == 1 ]] && RLTEST_ARGS+=" -i"
	RLTEST_ARGS+=" -v -s --test $TEST"
	export RUST_BACKTRACE=1
fi

SHARDS=${SHARDS:-3}

[[ $SAN == addr ]] && SAN=address
[[ $SAN == mem ]] && SAN=memory

RLEC_PORT=${RLEC_PORT:-12000}

[[ $PARALLEL == 1 ]] && RLTEST_PARALLEL="--parallelism $($READIES/bin/nproc)"

#---------------------------------------------------------------------------------------------- 

if [[ -n $SAN ]]; then
	if ! grep THPIsEnabled /build/redis.blacklist &> /dev/null; then
		echo "fun:THPIsEnabled" >> /build/redis.blacklist
	fi

	# for module
	export RS_GLOBAL_DTORS=1

	# for RLTest
	export SANITIZER="$SAN"
	export SHORT_READ_BYTES_DELTA=512
	
	SAN_ARGS="--no-output-catch --exit-on-failure --check-exitcode --unix"

	if [[ $SAN == addr || $SAN == address ]]; then
		REDIS_SERVER=${REDIS_SERVER:-redis-server-asan-6.2}
		if ! command -v $REDIS_SERVER > /dev/null; then
			echo Building Redis for clang-asan ...
			$READIES/bin/getredis --force -v 6.2 --no-run --suffix asan --clang-asan --clang-san-blacklist /build/redis.blacklist
		fi

		export ASAN_OPTIONS=detect_odr_violation=0
		# :detect_leaks=0

	elif [[ $SAN == mem || $SAN == memory ]]; then
		REDIS_SERVER=${REDIS_SERVER:-redis-server-msan-6.2}
		if ! command -v $REDIS_SERVER > /dev/null; then
			echo Building Redis for clang-msan ...
			$READIES/bin/getredis --force -v 6.2  --no-run --suffix msan --clang-msan --llvm-dir /opt/llvm-project/build-msan --clang-san-blacklist /build/redis.blacklist
		fi
	fi

elif [[ $VG == 1 ]]; then
	REDIS_SERVER=${REDIS_SERVER:-redis-server-vg}
	if ! is_command $REDIS_SERVER; then
		echo Building Redis for Valgrind ...
		$READIES/bin/getredis -v 6 --valgrind --suffix vg
	fi
	VALGRIND_ARGS=--use-valgrind
	if [[ $VG_LEAKS == 0 ]]; then
		export VG_OPTIONS="--leak-check=no --track-origins=yes --suppressions=$ROOT/tests/valgrind/redis_valgrind.sup"
		VALGRIND_ARGS+=" --vg-no-leakcheck --vg-options=\"--leak-check=no --track-origins=yes --suppressions=$ROOT/tests/valgrind/redis_valgrind.sup\" "
	fi

	# for module
	export RS_GLOBAL_DTORS=1

	# for RLTest
	export VALGRIND=1
	export SHORT_READ_BYTES_DELTA=512

else
	REDIS_SERVER=${REDIS_SERVER:-redis-server}
fi

if ! is_command $REDIS_SERVER; then
	echo "Cannot find $REDIS_SERVER. Aborting."
	exit 1
fi

#---------------------------------------------------------------------------------------------- 

if [[ $COV == 1 ]]; then
	COV_ARGS="--unix"
	
	export CODE_COVERAGE=1
	export RS_GLOBAL_DTORS=1
fi

#---------------------------------------------------------------------------------------------- 

if [[ $REDIS_VERBOSE == 1 || $VERBOSE ]]; then
    RLTEST_ARGS+=" -s -v"
fi

#---------------------------------------------------------------------------------------------- 

run_tests() {
	local title="$1"
	shift
	if [[ -n $title ]]; then
		$READIES/bin/sep -0
		printf "Running $title:\n\n"
	fi

	if [[ $EXISTING_ENV != 1 ]]; then
		rltest_config=$(mktemp "${TMPDIR:-/tmp}/rltest.XXXXXXX")
		rm -f $rltest_config
		cat <<-EOF > $rltest_config
			--oss-redis-path=$REDIS_SERVER
			--module $MODULE
			--module-args '$MODARGS'
			$RLTEST_ARGS
			$RLTEST_PARALLEL
			$VALGRIND_ARGS
			$SAN_ARGS
			$COV_ARGS

			EOF

	else # existing env
		xredis_conf=$(mktemp "${TMPDIR:-/tmp}/xredis_conf.XXXXXXX")
		rm -f $xredis_conf
		cat <<-EOF > $xredis_conf
			loadmodule $MODULE $MODARGS
			EOF

		rltest_config=$(mktemp "${TMPDIR:-/tmp}/xredis_rltest.XXXXXXX")
		rm -f $rltest_config
		cat <<-EOF > $rltest_config
			--env existing-env
			$RLTEST_ARGS

			EOF

		if [[ $VERBOSE == 1 ]]; then
			echo "External redis-server configuration:"
			cat $xredis_conf
		fi

		$REDIS_SERVER $xredis_conf &
		XREDIS_PID=$!
		echo "External redis-server pid: " $XREDIS_PID
	fi

	# Use configuration file in the current directory if it exists
	if [[ -n $CONFIG_FILE && -e $CONFIG_FILE ]]; then
		cat $CONFIG_FILE >> $rltest_config
	fi

	if [[ $VERBOSE == 1 ]]; then
		echo "RLTest configuration:"
		cat $rltest_config
	fi

	local E=0
	if [[ $NOP != 1 ]]; then
		{ $OP python2 -m RLTest @$rltest_config; (( E |= $? )); } || true
	else
		$OP python2 -m RLTest @$rltest_config
	fi
	rm -f $rltest_config

	if [[ -n $XREDIS_PID ]]; then
		echo "killing external redis-server: $XREDIS_PID"
		kill -9 $XREDIS_PID
	fi

	return $E
}

#---------------------------------------------------------------------------------------------- 

E=0

if [[ -z $COORD ]]; then
	{ (run_tests "RediSearch tests"); (( E |= $? )); } || true

elif [[ $COORD == oss ]]; then
	oss_cluster_args="--env oss-cluster --env-reuse --clear-logs --shards-count $SHARDS"

	{ (MODARGS+=" PARTITIONS AUTO" RLTEST_ARGS+=" ${oss_cluster_args}" \
	   run_tests "OSS cluster tests"); (( E |= $? )); } || true

	if [[ $QUICK != 1 ]]; then
		{ (MODARGS+=" PARTITIONS AUTO; OSS_GLOBAL_PASSWORD password;" \
		   RLTEST_ARGS+=" ${oss_cluster_args} --oss_password password" \
		   run_tests "OSS cluster tests with password"); (( E |= $? )); } || true
		{ (MODARGS+=" PARTITIONS AUTO SAFEMODE" RLTEST_ARGS+=" ${oss_cluster_args}" \
		   run_tests "OSS cluster tests (safe mode)"); (( E |= $? )); } || true
	fi # QUICK

elif [[ $COORD == rlec ]]; then
	dhost=$(echo "$DOCKER_HOST" | awk -F[/:] '{print $4}')
	{ (RLTEST_ARGS+=" --env existing-env --existing-env-addr $dhost:$RLEC_PORT" run_tests "tests on RLEC"); (( E |= $? )); } || true
fi

exit $E
