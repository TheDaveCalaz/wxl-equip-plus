# Third-party: Kirazul's port of retail's physics engine

`physics_domino.h` and `physics_domino.cpp` are copied unchanged from
[Kirazul/wow-retail-backport](https://github.com/Kirazul/wow-retail-backport)
(`scripts/ref/`, commit d086009), with the author's permission. They are Kirazul's
port of the retail client's runtime physics engine ("Domino"), read from the
retail client; see the files' own headers and the kit's `references/rules.md`
section 6. They are not covered by this repository's GPL licence statement:
the copyright position of the original engine is Blizzard's, and the port is
Kirazul's work.

Equip Plus only adds the glue that feeds the client's bone matrices in and
writes the simulated bones back (`../PhysicsLive.cpp`).
