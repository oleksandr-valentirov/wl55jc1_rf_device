CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g \
         -I../Core/Inc -I../radio_stack/inc -I../radio_stack/profiles \
         -I../radio_stack/test/vectors -I. \
         -DRADIO_PROFILE=RADIO_PROFILE_ASBUILT

# The library's own tests live in radio_stack/test and run under both profiles.
# What is here is this tree's: the fixture, the store, the console's readings,
# and the one comparison of the reference deck against the library's.
# radio_devices_docs/specs/03-roadmap.md

# This side's supply of the schedule's two hash operations, and the schedule the
# fixture drives. Deleting exchange.c in the submodule fails this suite.
SESSION_LAYER = ../radio_stack/src/exchange.c ../Core/Src/kdf.c \
                ../Core/Src/sha256.c

TESTS = test_fingerprint test_hop test_hublogic test_invite test_link \
        test_telemetry test_upseq test_uptime

# A change to a vector header must rebuild the tests that check it. Without
# this the binaries stayed up to date against their own sources while the
# contract moved underneath, and the suite went green printing the previous
# digests - the exact failure that including the hub's headers rather than
# copying them exists to prevent.
#
# Listed explicitly rather than via -MMD: one cc invocation per test compiles
# several sources and every one of them writes the same .d file, so only the
# last source's headers survive. That form looks like dependency tracking and
# tracks one translation unit.
VECTORS = $(wildcard ../radio_stack/test/vectors/*.h) \
          $(wildcard ../radio_stack/inc/*.h) \
          $(wildcard ../radio_stack/profiles/*.h) \
          $(wildcard ../Core/Inc/*.h)

$(TESTS): $(VECTORS)

test_telemetry: test_telemetry.c host_timebase.c ../Core/Src/telemetry.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_uptime: test_uptime.c host_timebase.c ../Core/Src/uptime.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# Both implementations in one binary: the reference this tree kept, and the
# library's. It is the only thing that compares them. ADR-0029 decision 5
test_hop: test_hop.c hop_reference.c ../radio_stack/src/hop.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_link: test_link.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# Header-only, so the arithmetic the loop runs is the arithmetic checked here.
test_upseq: test_upseq.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_fingerprint: test_fingerprint.c host_timebase.c ../Core/Src/pairing.c ../Core/Src/sha256.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# The curve is real and the AEAD is not; host_crypto.c says why, and test_link
# is where a sealed frame is checked.
test_hublogic: test_hublogic.c fake_phy.c host_crypto.c host_timebase.c \
               ../Core/Src/hublogic.c $(SESSION_LAYER) \
               ../radio_stack/src/grid.c ../radio_stack/src/gridmaster.c \
               ../third_party/monocypher/monocypher.c
	$(CC) $(CFLAGS) -I../third_party/monocypher -o $@ $(filter %.c,$^)

# The curve is stubbed to abort rather than linked: see host_curve_absent.c.
test_invite: test_invite.c host_timebase.c host_curve_absent.c ../Core/Src/pair_init.c ../Core/Src/sha256.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

.PHONY: check clean
check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -f $(TESTS)
