"""Canonical issuer and explicit signing key configuration; never publish secrets."""
import json
from pathlib import Path
from urllib.parse import urlsplit
from functools import lru_cache
import jwt
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from app.config import get_settings


def canonical_issuer(settings=None):
    s = settings or get_settings()
    value = (s.issuer or s.public_url or '').rstrip('/')
    url = urlsplit(value)
    if url.scheme not in ('http', 'https') or not url.netloc or url.query or url.fragment or url.username:
        raise ValueError('Configure an absolute canonical UNISSO_ISSUER URL')
    if not s.debug and url.scheme != 'https':
        raise ValueError('Production issuer must use HTTPS')
    return value


@lru_cache(maxsize=8)
def _load_rsa(private_path, public_path):
    private = serialization.load_pem_private_key(Path(private_path).read_bytes(), password=None)
    if not isinstance(private, rsa.RSAPrivateKey) or private.key_size < 2048:
        raise ValueError('An RSA private key of at least 2048 bits is required')
    public = private.public_key()
    if public_path:
        configured = serialization.load_pem_public_key(Path(public_path).read_bytes())
        if not isinstance(configured, rsa.RSAPublicKey) or configured.public_numbers() != public.public_numbers():
            raise ValueError('Signing and public keys do not match')
    return private, public


def signing_material(settings=None):
    s = settings or get_settings()
    if s.jwt_algorithm == 'HS256':
        if len(s.secret_key) < 32:
            raise ValueError('HS256 requires a strong secret of at least 32 characters')
        return s.secret_key, s.secret_key
    if s.jwt_algorithm != 'RS256':
        raise ValueError('Only explicitly configured RS256 or transitional HS256 is supported')
    return _load_rsa(s.signing_private_key_file, s.signing_public_key_file)


def validate_signing_configuration():
    s = get_settings()
    canonical_issuer(s)
    signing_material(s)
    if not s.signing_key_id:
        raise ValueError('A signing key ID is required')


def public_jwks():
    s = get_settings()
    if s.jwt_algorithm == 'HS256':
        return {'keys': []}
    _, public = signing_material(s)
    jwk = json.loads(jwt.algorithms.RSAAlgorithm.to_jwk(public))
    jwk.update(kid=s.signing_key_id, use='sig', alg='RS256')
    return {'keys': [jwk]}
