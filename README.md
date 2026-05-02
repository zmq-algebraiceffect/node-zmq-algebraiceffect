# node-zmq-algebraiceffect

Node.js native addon for the [zmq-algebraiceffect](https://github.com/zmqae/zmq-algebraiceffect) protocol.

## Install

```bash
git clone --recursive https://github.com/zmqae/node-zmq-algebraiceffect.git
cd node-zmq-algebraiceffect
npm install
```

## Usage

```js
const { Client, Router } = require('zmq-algebraiceffect');

const client = new Client('tcp://localhost:5555');
const result = await client.perform('Transcribe', { audio: '...' });
// result: { id, value, error? }

await client.perform('Effect', payload, 5000); // timeout ms
client.close();

const router = new Router('tcp://*:5555');
router.on('Transcribe', (ctx) => {
  ctx.payload; // parsed JSON
  ctx.resume({ text: 'hello' });
  // or ctx.error('fail');
});
router.close();
```

## Build from source

Requires: Node.js 20+, CMake 3.14+

```bash
git submodule update --init --recursive
npm install
```

## Test

```bash
npm test
```

## License

MIT
