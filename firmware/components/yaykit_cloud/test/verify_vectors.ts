// Checks signatures from sign_vectors.c (ESP-IDF's mbedTLS, DER) with the device API's own verifier,
// and that a changed message is refused. Usage: tsx verify_vectors.ts < vectors.json
import { readFileSync } from 'node:fs';
import { verifyDeviceSignature } from '../../../../packages/cloud/src';

const vectors = JSON.parse(readFileSync(0, 'utf8')) as {
  publicKey: string;
  activation: { message: string; signature: string };
  token: { message: string; signature: string };
};
let failures = 0;
for (const [name, v] of Object.entries({ activation: vectors.activation, token: vectors.token })) {
  const ok = await verifyDeviceSignature(vectors.publicKey, v.message, v.signature);
  const tampered = await verifyDeviceSignature(vectors.publicKey, `${v.message}x`, v.signature);
  console.log(`${name}: verified=${ok} tamperedRejected=${!tampered}`);
  if (!ok || tampered) failures++;
}
process.exit(failures ? 1 : 0);
