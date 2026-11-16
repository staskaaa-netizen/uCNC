# Encoder Follower Testing

Software verification:

- Build the active NCC target.
- Confirm the module is loaded after the RP2350 encoder backend.
- Confirm a rejected planner move is not retained and replayed.

Bench verification:

- Leave the machine idle and turn ENC1 one count; X should request one motor
  step by default.
- Turn ENC1 continuously: the follower must keep issuing jog blocks after the
  first block changes the controller state to `EXEC_JOG | EXEC_RUN`.
- The diagnostic line reports `F1:t<ticks> q<samples> e<position> d<delta>
  s<steps> st<status> x<state> a<accepted> r<rejected>`; use it to separate a
  missing listener, rejected state, rejected jog request or STEP-output fault.
- Turn ENC2 one count; Z should request one motor step.
- Turn both and verify they arrive as one interpolated X/Z move.
- Enter alarm, turn both encoders, then unlock; neither axis may jump on unlock.
- Start a program, turn both encoders during the run, and verify the run is not
  perturbed.
