/* Host-only symbols the firmware gets from the board.

   The virtual-MCU host build has no kinematics provider, so the desktop shell
   links the explicit Cartesian identity the parser test also uses. */

void kinematics_apply_transform(float *axis)
{
    (void)axis;
}

void kinematics_apply_reverse_transform(float *axis)
{
    (void)axis;
}
