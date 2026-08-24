CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g \
         -I../Core/Inc -I../../OpenHub/Common/inc \
         -I../../OpenHub/Common/test/vectors -I.

# The link layer is one set of sources and they live in the hub's tree until
# phase 9 step 6. Deleting them there fails this suite.
LINK_LAYER = ../../OpenHub/Common/src/grid.c ../../OpenHub/Common/src/superframe.c \
             ../../OpenHub/Common/src/beacon.c

TESTS = test_beacon test_exchange test_fingerprint test_hop test_hublogic \
        test_invite test_link test_period test_telemetry test_uptime

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
VECTORS = $(wildcard ../../OpenHub/Common/test/vectors/*.h) \
          $(wildcard ../../OpenHub/Common/inc/*.h) \
          $(wildcard ../Core/Inc/*.h)

$(TESTS): $(VECTORS)

test_beacon: test_beacon.c host_timebase.c $(LINK_LAYER)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_telemetry: test_telemetry.c host_timebase.c ../Core/Src/telemetry.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_uptime: test_uptime.c host_timebase.c ../Core/Src/uptime.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_hop: test_hop.c ../Core/Src/hop.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_period: test_period.c host_timebase.c $(LINK_LAYER)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_link: test_link.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_exchange: test_exchange.c host_timebase.c ../Core/Src/exchange.c ../Core/Src/sha256.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_fingerprint: test_fingerprint.c host_timebase.c ../Core/Src/pairing.c ../Core/Src/sha256.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

# The curve is real and the AEAD is not; host_crypto.c says why, and test_link
# is where a sealed frame is checked.
test_hublogic: test_hublogic.c fake_phy.c host_crypto.c host_timebase.c \
               ../Core/Src/hublogic.c ../Core/Src/exchange.c ../Core/Src/sha256.c \
               ../../OpenHub/Common/src/grid.c ../../OpenHub/Common/src/gridmaster.c \
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
