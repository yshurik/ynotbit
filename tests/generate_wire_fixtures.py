# Independent construction following PyBitmessage v0.6 class_singleWorker.py.
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes, serialization, padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import hashlib, hmac, struct, sys
K=lambda n: ec.derive_private_key(n, ec.SECP256K1())
P=lambda k: k.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
S=lambda b: hashlib.sha512(b).digest()
def var(n):
    if n<253:return bytes([n])
    return (b'\xfd'+struct.pack('>H',n)) if n<65536 else b'\xfe'+struct.pack('>I',n)
def blob(b):return var(len(b))+b
def header(t,v):return bytes(8)+struct.pack('>QI',1700000000,t)+var(v)+b'\x01'
def ripe(s,e):return hashlib.new('ripemd160',S(P(s)+P(e))).digest()
def enc(data,pub):
    ephemeral=K(5);iv=bytes(range(16)); shared=S(ephemeral.exchange(ec.ECDH(),pub))
    pubbytes=P(ephemeral); out=iv+b'\x02\xca\x00\x20'+pubbytes[1:33]+b'\x00\x20'+pubbytes[33:]
    pad=padding.PKCS7(128).padder(); padded=pad.update(data)+pad.finalize()
    c=Cipher(algorithms.AES(shared[:32]),modes.CBC(iv)).encryptor()
    out+=c.update(padded)+c.finalize();return out+hmac.new(shared[32:],out,hashlib.sha256).digest()
def sign(data,sha1=False):return K(1).sign(data, ec.ECDSA(hashes.SHA1() if sha1 else hashes.SHA256(), deterministic_signing=True))
def fields(v):return struct.pack('>I',1)+P(K(1))[1:]+P(K(2))[1:]+(var(1000)*2 if v>=3 else b'')
text=b'Subject:Reference\nBody:Independent Python fixture'
ack=header(2,1)+bytes(range(32)); frame=b'\xe9\xbe\xb4\xd9object'+bytes(6)+struct.pack('>I',len(ack))+S(ack)[:4]+ack
fixtures={}
for label, ackframe, dest, ver in [('referenceMessage',frame,ripe(K(3),K(4)),b'\x03'),('badAckMessage',frame[:20]+bytes(4)+frame[24:],ripe(K(3),K(4)),b'\x03'),('wrongRipeMessage',frame,bytes(20),b'\x03'),('noncanonicalMessage',frame,ripe(K(3),K(4)),b'\xfd\x00\x03')]:
    h=header(2,1);p=ver+b'\x01'+fields(3)+dest+b'\x02'+blob(text)+blob(ackframe)
    fixtures[label]=h+enc(p+blob(sign(h[8:]+p,True)), K(4).public_key())
for v in [2,3,4]:
    h=header(1,v);p=fields(v)
    if v==3:p+=blob(sign(h[8:]+p,True))
    if v==4:
        d=S(S(bytes([v,1])+ripe(K(1),K(2))));h+=d[32:];p=enc(p+blob(sign(h[8:]+p)),K(int.from_bytes(d[:32],'big')).public_key())
    fixtures['referencePubkey'+str(v)]=h+p
    h=header(3,4 if v<4 else 5);d=S(bytes([v,1])+ripe(K(1),K(2)))
    if v==4:d=S(d);h+=d[32:]
    p=bytes([v,1])+fields(v)+b'\x02'+blob(text)
    fixtures['referenceBroadcast'+str(v)]=h+enc(p+blob(sign(h[8:]+p)),K(int.from_bytes(d[:32],'big')).public_key())
with open(sys.argv[1] if len(sys.argv)>1 else 'fixtures.inc','w') as f:
    f.write('// Fixed independent Python cryptography fixtures. Scalars 1/2 sender, 3/4 recipient,\n// ephemeral scalar 5, IV 000102...0f, timestamp 1700000000. SHA1 message/v3 pubkey.\n')
    for name,b in fixtures.items():
        f.write('static const QByteArray '+name+' = QByteArray::fromHex(\n')
        for i in range(0,len(b.hex()),96):f.write('    "'+b.hex()[i:i+96]+'"\n')
        f.write(');\n')
