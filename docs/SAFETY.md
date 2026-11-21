# Safety

A 65 A ESC with a propeller on the shaft is a tool capable of causing a
serious injury. The software protections below are needed, but they are
secondary to physical protection.

## The physical part

- **A barrier in the plane of the propeller.** A blade coming apart is the
  main danger. A polycarbonate screen or steel mesh between the propeller and
  the operator.
- **Never stand in the plane of rotation.** Stand to the side, not in front
  and not behind.
- **Check the tightness** of the propeller nut and of the motor mount before
  every run. Vibration undoes everything.
- **Clear loose objects** off the table: rags, cable ties and wires get pulled
  into the propeller.
- **The battery through a fuse or a current limiter**, rated deliberately for
  the test regime.
- **The first runs go without a propeller.** The link, the protocol and the
  direction of rotation are checked on a bare shaft.

## The dead-man chain

Stopping the motor takes no active effort, it happens by itself as soon as
something in the chain falls silent:

| Link | Period | What it does on silence |
|---|---|---|
| Browser | sends a heartbeat every 100 ms | — |
| Server | relays `K` 20 times per second | stops sending if there has been no heartbeat from the browser for longer than 400 ms |
| Firmware | waits for any command | kills the motor and latches a fault if the silence lasts longer than 300 ms |

That is, the motor stops when the tab is closed, when the browser hangs, when
Wi-Fi is lost, when the laptop goes to sleep, when the server dies, when the
USB cable is unplugged.

The tab moving into the background is handled separately: browsers slow their
timers down in hidden tabs, so the page removes the throttle deliberately
rather than letting it come to the watchdog. The motor stops normally, with no
fault latched.

## Software protections

| Protection | Setting | Behaviour |
|---|---|---|
| Throttle limit | "Throttle limit, %" | commands above the limit are clipped in the firmware |
| Slew-rate limit | "Ramp, %/s" | the throttle cannot change in a step |
| Current cutoff | "Current cutoff, A" | the throttle drops instantly and the fault latches |
| Temperature cutoff | "ESC t° cutoff" | the same |
| Emergency stop | spacebar or the STOP button | the throttle drops instantly, bypassing the ramp |

The current cutoff compares the threshold against the larger of two
independent measurements, the ESC data and the sensor on the board. A zero in
the field disables the protection.

A fault **latches**: until the operator presses "Clear fault", the throttle
cannot be enabled again. This was done deliberately, so that a failure cannot
be slipped past unnoticed.

## First power-up

Neither the firmware nor the server has been checked on real hardware yet. Go
through the steps without skipping any.

1. **No propeller, no battery.** Flash the board, plug in USB, open the
   interface. Make sure the green dot is lit and telemetry is coming in.
2. **No propeller, with the battery.** Voltage and ESC temperature will
   appear. Press "Beep the motor", the ESC should answer.
3. **No propeller, low throttle.** ARM, throttle 5–10 %. The shaft should turn
   smoothly, and the rpm in the interface should rise in proportion to the
   throttle. Check the direction of rotation.
4. **Check the emergency stop** on a running motor: press the spacebar. The
   motor must stop immediately and the fault banner must appear.
5. **Check the loss of the link**: at low throttle, pull the USB cable out.
   The motor must stop. This is the most important check of them all, do it
   without fail and do it without fail with no propeller.
6. **Only now fit the propeller** and start at 10–20 % throttle.

## If the rpm looks implausible

Most likely the number of motor magnets is set wrong. Current and voltage stay
correct while the rpm is off by an integer factor. The magnets have to be
counted on the rotor (inside the bell), not the teeth of the stator.

## If the motor does not start

First check whether the protocol was changed without power-cycling the ESC,
AM32 remembers the protocol until it reboots, see
[ESC_AM32.md](ESC_AM32.md). This is the most frequent cause.
