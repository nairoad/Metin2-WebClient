"""SpeedTree leaf-card measurement: derives each card's rotation
axis angle rho from the elevation dumps and checks the model
corner(az, el) = Rz(az) Rz(rho) Ry(-el) Rz(-rho) corner(0, 0) against every
dump, printing the worst error.
"""
import numpy as np, math, glob
np.set_printoptions(precision=3, suppress=True, linewidth=150)
def read_file(p):
    """The `U card corner x y z` lines of a dump file as {(card, corner): xyz}."""
    U = {}
    for l in open(p):
        w = l.split()
        if w and w[0] == 'U':
            U[(int(w[1]), int(w[2]))] = np.array(list(map(float, w[3:6])))
    return U
def Rz(a):
    """Rotation matrix about Z by `a` radians."""
    c, s = math.cos(a), math.sin(a); return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
def Ry(a):
    """Rotation matrix about Y by `a` radians."""
    c, s = math.cos(a), math.sin(a); return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
elevations = [-90, -75, -60, -45, -30, -15, 0, 15, 30, 45, 60, 75, 90]
T = {e: read_file('el_%d.txt' % e) for e in elevations}
base = T[0]
# rho_k from the fit a+b cos+c sin: a lies on the rotation axis
rho = {}
for k in range(4):
    X = np.array([[1, math.cos(math.radians(e)), math.sin(math.radians(e))] for e in elevations])
    Y = np.array([T[e][(k, 0)] for e in elevations])
    C, _, _, _ = np.linalg.lstsq(X, Y, rcond=None)
    a = C[0]
    # axis n = +-a/|a| in the XY plane; I take the one with positive y
    n = a / np.linalg.norm(a)
    if n[1] < 0: n = -n
    rho[k] = math.atan2(n[1], n[0]) - math.pi / 2
    print('cluster %d: axis %s rho %.2f deg' % (k, n, math.degrees(rho[k])))
# model: corner(az, el) = Rz(az) Rz(rho) Ry(-el) Rz(-rho) corner(0,0)
def model(k, r, az, elevation):
    """Predicted corner `r` of card `k` at azimuth `az` and elevation `elevation`:
    Rz(az) Rz(rho) Ry(-el) Rz(-rho) applied to the corner at (0, 0).
    """
    return Rz(az) @ Rz(rho[k]) @ Ry(-elevation) @ Rz(-rho[k]) @ base[(k, r)]
worse = 0
for e in elevations:
    for k in range(4):
        for r in range(4):
            worse = max(worse, np.abs(model(k, r, 0, math.radians(e)) - T[e][(k, r)]).max())
print('az=0, all elevations: max error %.4f' % worse)
for p in sorted(glob.glob('kam_*.txt')):
    d = np.array([float(x) for x in p[4:-4].split('_')])
    if np.linalg.norm(d) == 0: continue
    az = math.atan2(d[1], d[0]); elevation = math.asin(d[2] / np.linalg.norm(d))
    U = read_file(p)
    e = max(np.abs(model(k, r, az, elevation) - U[(k, r)]).max() for k in range(4) for r in range(4))
    print('%s az %.1f el %.1f: max error %.4f' % (p, math.degrees(az), math.degrees(elevation), e))
