def MontgomeryCurve (*args):
  """
  MontgomeryCurve ([A,B])
  MontgomeryCurve (Ring, [A,B])
  """
  if len(args) == 1:
    A = args[0][0]
    B = args[0][1]
    return EllipticCurve ([0,A/B,0,1/B^2,0])
  elif len(args) == 2:
    R = args[0]
    A = args[1][0]
    B = args[1][1]
    return EllipticCurve (R, [0,A/B,0,1/B^2,0])
  else:
    raise TypeError ("MontgomeryCurve takes at most 2 arguments")

def _Suyama_parameterization_internal (s):
  u = s^2-5
  v = 4*s
  x0 = u^3
  y0 = (s^2-1)*(s^2-25)*(s^4-25)
  z0 = v^3
  A = (v-u)^3*(3*u+v)/(4*u^3*v) - 2
  B = u/z0
  b = (v-u)^3*(3*u+v)/(16*u^3*v)                          # [ b = (A+2)/4 ]
  x3 = u
  y3 = 2*(s^2-25)*(s^2-1)*s/u
  z3 = v
  return A,B,b,x0,y0,z0,x3,y3,z3

def _Suyama_parameterization_check ():
  s = var('s')
  A,B,b,x0,y0,z0,x3,y3,z3 = _Suyama_parameterization_internal (s)
  assert (b-(A+2)/4).is_zero(), "b is not correct"
  assert (B*y0^2*z0 - (x0^3+A*x0^2*z0+x0*z0^2)).is_zero(), "P0 not on the curve"
  assert (B*y3^2*z3 - (x3^3+A*x3^2*z3+x3*z3^2)).is_zero(), "P3 not on the curve"
  assert (4*A*(x3/z3)^3+3*(x3/z3)^4+6*(x3/z3)^2-1).is_zero(), "not of order 3"

def Suyama_parameterization (*args, **kwargs):
  if len(args) == 1:
    s = args[0]
    curve_args = []
  elif len(args) == 2:
    s = args[1]
    curve_args = [args[0]]
  else:
    raise TypeError ("Suyama_parameterization takes at most 2 arguments")

  A,B,_,x0,y0,z0,x3,y3,z3 = _Suyama_parameterization_internal (s)
  curve_args.append([A,B])
  E = MontgomeryCurve (*curve_args)
  P0 = E(x0,y0,B*z0)
  if kwargs.get("check", False):
    P3 = E(x3,y3,B*z3)
    assert P3.order() == 3, "P3 is not of order 3"
  return E, P0

def _Z6_parameterization_internal (k):
  Eg = EllipticCurve (QQ, [-9747, 285714])
  Pg = Eg(15, 378)
  Qg = k*Pg
  p = Qg[0]/Qg[2]
  q = Qg[1]/Qg[2]

  sigma = -(p-213)/(p+3)

  alpha = (p-105)*(p+57)
  beta = q*(p-213)
  gamma = p^4 - 96*p^3 - 918*p^2 - 2808*p + 45346797
  delta = gamma + 2^5*3^7 *(p-33)^2
  epsilon =  p^2 - 66*p + 7569
  zeta = 2^2*3^6*q*(p-33)

  # Coeffs for "a=-1" Twisted Edwards curve
  a = -1
  d = -alpha^3*(p-51)*(p+327)/zeta^2
  xE0 = 2*3*(p+3)*alpha*beta*delta
  yE0 = (2*3^5)*(p-33)*alpha*gamma*epsilon
  zE0 = alpha^2*delta*epsilon
  tE0 = 2^2*3^6*(p-33)*(p+3)*beta*gamma
  xE3 = 2*3^2*q*alpha
  yE3 = 2*3^4*(p-33)*alpha
  zE3 = alpha^2
  tE3 = zeta
  
  # Coeffs for Montgomery curve
  A = 2*(a+d)/(a-d)
  B = 4/(a-d)
  b = zeta^2/(zeta^2-alpha^3*(p-51)*(p+327))               # [ b = (A+2)/4 ]
  xM0 = (p^2 + 114*p - 11331)^3
  yM0 = (alpha*epsilon*(epsilon+2^2*3^2*5*(p-105))^3)/(2*3*(p+3)*beta)
  zM0 = ((p-213)*(p+3))^3
  xM3 = 2*3^2*q*(epsilon+2^2*3^2*5*(p-105))
  yM3 = alpha*(epsilon+2^2*3^2*5*(p-105))
  zM3 = 2*3^2*(p+3)*beta

  Ed = (a, d, xE0, yE0, zE0, tE0, xE3, yE3, zE3, tE3)
  Mt = (A, B, b, xM0, yM0, zM0, xM3, yM3, zM3)
  return sigma, Ed, Mt



# Brent-Sumaya parameterization.
# Given rational parameter sigma, compute an elliptic curve in Weierstrass
# form together with a non torsion point.
def BrentSuyama_parameterization (sigma, p) :
    K = GF(p)
    u = K(sigma^2 - 5)
    v = K(4*sigma)
    a = (v - u)^3 * (3*u + v)
    b = 4*u^3*v
    A = a/b - 2
    assert (A != 2 and A != -2)
    x = u^3 / v^3
    w = x^3 + A*x^2 + x
    assert (w != 0)
    E = EllipticCurve (K, [0, w*A, 0, w^2, 0])
    return [E, E.point([x * w, w^2])]

# Montgomery elliptic parameterization with torsion group of order 12.
# Given integer parameter n, compute  an elliptic curve in Montgomery form
# with torsion subgroup of order 12.
# Returns an equivalent curve in short Weierstrass form.
def Monty12_parameterization (n, p, verbose=false) :
    K = GF(p)
    # v^2 = u^3 - 12u with generator (-2, 4)
    E = EllipticCurve (K, [0,0,0,-12,0])
    P0 = E.point([-2,4])
    [u,v,w] = n*P0
    if (verbose) :
        print u, ":", v
    t = v/(2*u)
    t2 = t^2
    assert (t2 != -3)
    a = (t2 - 1)/(t2 + 3)
    assert (a != 0)
    X0 = 3*a^2+1
    Z0 = 4*a
    A = -(3*a^4+6*a^2-1)/(4*a^3)
    assert (A != 2 or A != -2)
    B = (a^2-1)^2/(4*a^3)
    assert (B != 0)

    # The curve BY^2Z = X(X^2 + AXZ + Z^2) has a torsion subgroup of order 12

    x = X0/Z0
    y = 2 * (u^2+12)/(4*u)  / ((t^2+3)*(4*a))
    Ew = EllipticCurve (K, [0,A*B,0,B^2, 0])
    xw = B*x
    yw = B^2*y
    assert (Ew.is_on_curve(xw, yw))

    if (verbose) :
        print "t^2 = ", t2
        print "a =   ", a
        print "X0 =", X0, "is square (mod",p,")? ", X0.is_square()
        print "A = ", A, ", B = ", B
        print "x = ", x, ", y = ", y
        print Ew
        print "12 | order? ", Ew.order()%12 == 0
        # (a,a) is a point of order 3
        P3 = Ew([a*B,a*B^2])
        print "P3 = ", P3
        print "[3]P3 = ", 3*P3
        
    return [Ew, Ew([xw,yw])]

# def Monty16_parameterization (n, p) :

# def Twed12_parameterization (n,p) :
    

# get_order

# P-1
# Inputs: p, param
# Output: [p, o, po, lpf]

# R = IntegerModRing (p-1)
# a = R(param)
# o = p-1
# po = a.order()
# lpf = largest_prime_factor(po)
# return [p, o, po, lpf]

def get_order_from_method(method, sigma, p) :
# ECM
# Inputs:
# Output: [p, o, po, lpf]

# Brent-Suyama (sigma)
# Compute elliptic curve E parameterized by sigma and base point (x,y)
    if (method == 2) :
        T = BrentSuyama_parameterization (sigma, p)
    elif (method == 3) :
        T = Monty12_parameterization (sigma, p)
        #elif (method == 4) :
        #    T = Monty16_parameteriztion (n, p)
        #elif (method == 5) :
        #    T = Twed12_parameteriation (n, p)
        
        # T = [Elliptic curve E in Weierstrass form, curve parameter, non torsion point on E]
        
    o = T[0].order()
    po = T[-1].order()
    lpf = largest_prime_factor (po)
    return (p, o, po, lpf)

    
def largest_prime_factor (n) :
    F = n.factor()
    return F[-1][0]
    
def is_powersmooth (fact, B):
  return all (p^e <= B for p,e in fact)

# Find the smallest prime p >= minp such that largest prime factor lpf
# in the order of the starting element for the factoring method satisfies
# minlpf <= lpf <= maxlpf, or if maxlpf == 0, minlpf <= lpf.
def prime_with_lpf_in_range (minp, minlpf, maxlpf, method, param):
  assert maxlpf == 0 or minlpf <= maxlpf, "maxlpf must be 0 or >= minlpf"
  p = next_prime (minp-1)
  while True:
    info = get_order_from_method (method, param, p)
    if minlpf <= info[3] and (maxlpf == 0 or info[3] <= maxlpf):
      return info
    p = next_prime (p)


# Find the smallest prime p >= minp such that the order of the starting element
# for the factoring method is B1,B2-powersmooth
def prime_with_B1_B2_smooth_order (minp, B1, B2, method, param):
  assert B2 > B1, "B2 must be > B1"
  p = next_prime (minp-1)
  while True:
    info = get_order_from_method (method, param, p)
    o = info[2]
    lpf = info[3]
    if B1 < lpf and lpf <= B2 and is_powersmooth(ZZ(o/lpf).factor(), B1):
      return info
    p = next_prime (p)


def factor_test_line (pdata, qdata, pcomment="", qcomment=""):
  if qdata[0] < pdata[0]: # Rename so that p refers to the smaller prime
    qdata, pdata, qcomment, pcomment = pdata, qdata, pcomment, qcomment
  p = pdata[0]
  q = qdata[0]
  N = p*q
  pcomment += " " if pcomment else ""
  qcomment += " " if qcomment else ""
  pcomment += "(order=%d, lpf=%d)" % (pdata[2], pdata[3])
  qcomment += "(order=%d, lpf=%d)" % (qdata[2], qdata[3])
  return "%d %d %d # %s, %s\n" % (N, p, q, pcomment, qcomment)


def make_factor_test (B1, B2, method, param, minq=40, minp=10000):
  with open ('toto', 'w') as outfile:
    header = "# Created with: make_factor_test (%d, %d, %d, %d, %d, %d)\n"
    outfile.write (header % (B1, B2, method, param, minq, minp))

    # Composite number < 2^32
    # A B1-smooth factor # FIXME does not test correctly B1-powersmoothness
    p1 = prime_with_lpf_in_range (minp, minq, B1, method, param)
    # A non-smooth cofactor
    q = prime_with_lpf_in_range (minp, B2+1, 0, method, param)
    outfile.write (factor_test_line (p1, q, "one B1-smooth factor", "one non-smooth cofactor"))

    # A B1, B2-smooth factor, but not B1-smooth
    p2 = prime_with_B1_B2_smooth_order (minp, B1, B2, method, param)
    outfile.write (factor_test_line (p2, q, "one B1,B2-smooth factor", "one non-smooth cofactor"))

    # A B1 and a B1,B2-smooth factor
    outfile.write (factor_test_line (p1, p2, "one B1-smooth factor", "one B1,B2-smooth factor"))

    # Find two B1-smooth factors with different power of 2 in the order
    # Backtracking does not work reliably for ECM as in the addition chain with a
    # point of small order, an addition may be used incorrectly where a doubling
    # would be required, causing a zero coordinate before a backtracking
    # checkpoint is reached.
    if method < 2: # ie P-1 or P+2
      p3 = p1
      while p1[2].valuation(2) == p3[2].valuation(2):
        p3 = prime_with_lpf_in_range (p3[0]+1, minq, B1, method, param);
      outfile.write (factor_test_line (p1, p3, " # Two B1-smooth factors with different power of 2 in the order", ""))

    ## XXX remove this test
    q1 = prime_with_lpf_in_range (minp, B1, B1+50, method, param)
    q2 = prime_with_lpf_in_range (minp, q1[3] + 500, B2, method, param)
    outfile.write (factor_test_line (q1, q2, " # Two B1,B2-smooth factors with LPF in differnet giant-steps", ""))

    for v in [33, 49, 65, 97, 127, 128, 200]:
      # A non-smooth cofactor such that the product is >2^v[i]
      q = prime_with_lpf_in_range (floor(2^v / p1[0]), B2+1, 0, method, param)
      outfile.write (factor_test_line (p1, q, "one B1-smooth factor", "one non-smooth cofactor"))
      q = prime_with_lpf_in_range (floor(2^v / p2[0]), B2+1, 0, method, param)
      outfile.write (factor_test_line (p2, q, "one B1,B2-smooth factor", "one non-smooth cofactor"))
    
make_factor_test (100, 1000, 2, 10)
#print "#"
#print make_factor_test (100, 1000, 0, 2)
