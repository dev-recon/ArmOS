# ArmOS user login profile.

ENV=${HOME}/.shrc
export ENV

if test -r "$ENV"; then
	. "$ENV"
fi
