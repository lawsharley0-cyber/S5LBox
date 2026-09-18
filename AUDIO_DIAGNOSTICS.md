# Guest audio diagnostic candidate

Based on d67834c. This is a buffering repair and diagnostic build, not a claim
that iPhone OS 3.1.3 guest sound has been reproduced.

The user reports that Test Sound is audible on an iPhone 17 Pro Max running iOS
26, while guest sound is silent. That proves the host output path can work;
it does not establish guest DMA activity, codec routing, sample packing or rate.

## Changes

- A full SPSC buffer drops the incoming word. The producer never changes the
  consumer cursor or overwrites samples the consumer may be reading.
- Test Sound renders on the AudioQueue consumer, outside the guest buffer.
  It cannot act as a second producer or inflate guest sample counters.
- The sound report separately counts guest I2S0 words received, nonzero words,
  buffered frames consumed, dropped words and underruns. Counts are cumulative
  during the current machine run, so compare before and during a ringtone.
- Partial AudioQueue startup failures dispose already allocated resources.
- FIFO writes retain upstream census semantics. Format derivation must not
  interpret FIFO data as audio configuration register writes.
- Tests use the telemetry API instead of the removed count field, check full
  queue recovery and ordering, and stress concurrent producer/consumer access.

## Device check

1. Keep existing machine data and install with the same normal provisioning.
2. Boot the guest. Note Performance & Sound Details before starting a ringtone.
3. Start a ringtone in the guest and reopen the report. Copy both readings.
4. Test Sound remains available, but is excluded from guest counters.

Unchanged received count means no new data reached the I2S0 sink. Increasing
received count with no new nonzero words means the sink received zeros.
Increasing nonzero words while silent requires investigating format, codec
routing and timing. Drops and underruns distinguish delivery imbalance. The
current output still assumes 44.1 kHz signed 16-bit stereo; that guest format
is not yet established. The counts are observations, not a format detector.

No Apple firmware, sound recordings, guest disks or packages are included.
Runtime JIT and private executable-memory entitlements remain excluded.
