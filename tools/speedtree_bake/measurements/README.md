# SpeedTree leaf card measurements

`probe2.cpp` - a native program (builds like the baker: `cl` x86 +
`SpeedTreeRT.lib`, the DLL alongside): `probe2 <spt> <rocking 0/1> <wind> <dx> <dy> <dz> <t...>`
prints the leaf cluster tables (`U cluster corner x y z`) after `SetCamera(dir)`.
The scripts (Python 3.10 + numpy) read the files `kam_<dx>_<dy>_<dz>.txt`
and `el_<degrees>.txt`:

* `frame.py`  - the cards in the camera frame (right/down/centre);
* `kabsch2.py` - fitting a rotation to the az0/el0 basis (showed: azimuth = Rz(az), elevation is NOT a rigid rotation of the whole table);
* `elevation.py`   - the decomposition `a + b cos(el) + c sin(el)` per corner (error 0);
* `model.py`  - the formula `Rz(az+rho_k)·Ry(-el)·Rz(-rho_k)·corner(0,0)` on 8 cameras (error < 0.001).

Result and consequences: the camera model is written down in `compat/speedtree_web.cpp`.
