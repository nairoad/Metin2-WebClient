"""SpeedTree leaf-card measurement: for every camera dump
kam_<x>_<y>_<z>.txt, fits the linear map from the base corners (az 0, el 0)
and compares it with candidate rotations Rz(az), Rz(az)Ry(+-el),
Rz(az)Ry(+-el/2), printing the error of each.
"""
import numpy as np, glob, math
np.set_printoptions(precision=4, suppress=True)
def read_file(p):
    """The `U card corner x y z` lines of a dump file as an array, sorted by
    (card, corner).
    """
    U = {}
    for l in open(p):
        w = l.split()
        if w and w[0] == 'U':
            U[(int(w[1]), int(w[2]))] = np.array(list(map(float, w[3:6])))
    k = sorted(U)
    return np.array([U[i] for i in k])
def Rz(a):
    """Rotation matrix about Z by `a` radians."""
    c, s = math.cos(a), math.sin(a); return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
def Ry(a):
    """Rotation matrix about Y by `a` radians."""
    c, s = math.cos(a), math.sin(a); return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
def Rx(a):
    """Rotation matrix about X by `a` radians."""
    c, s = math.cos(a), math.sin(a); return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
A = read_file('kam_1_0_0.txt')  # az 0, el 0 = the base
for p in sorted(glob.glob('kam_*.txt')):
    d = np.array([float(x) for x in p[4:-4].split('_')])
    if np.linalg.norm(d) == 0: continue
    B = read_file(p)
    M, res, _, _ = np.linalg.lstsq(A, B, rcond=None)   # B = A @ M  -> per column: b = M.T @ a
    R = M.T
    err = np.abs(A @ M - B).max()
    az = math.atan2(d[1], d[0]); elevation = math.asin(max(-1, min(1, d[2] / np.linalg.norm(d))))
    print(p, 'az %.1f el %.1f  fit error %.4f det %.4f' % (math.degrees(az), math.degrees(elevation), err, np.linalg.det(R)))
    print(R)
    for name, K in (('Rz(az)', Rz(az)), ('Rz(az)Ry(-el)', Rz(az) @ Ry(-elevation)), ('Rz(az)Ry(el)', Rz(az) @ Ry(elevation)),
                     ('Rz(az)Ry(-el/2)', Rz(az) @ Ry(-elevation / 2)), ('Rz(az)Ry(el/2)', Rz(az) @ Ry(elevation / 2))):
        e = np.abs((K @ A.T).T - B).max()
        print('   %-18s error %.3f' % (name, e))
