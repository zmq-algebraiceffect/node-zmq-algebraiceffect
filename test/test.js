const { Client, Router } = require('..');

const ENDPOINT = 'tcp://127.0.0.1:59876';
const TIMEOUT = 5000;

async function main() {
    let passed = 0;
    let failed = 0;

    function assert(cond, msg) {
        if (cond) {
            console.log(`  PASS: ${msg}`);
            passed++;
        } else {
            console.error(`  FAIL: ${msg}`);
            failed++;
        }
    }

    console.log('=== zmq-algebraiceffect integration tests ===\n');

    const router = new Router(ENDPOINT);
    await new Promise(r => setTimeout(r, 200));

    console.log('1. Basic perform/resume');
    {
        router.on('Echo', (ctx) => {
            ctx.resume(ctx.payload);
        });

        const client = new Client(ENDPOINT);
        await new Promise(r => setTimeout(r, 100));

        const result = await client.perform('Echo', { hello: 'world' }, TIMEOUT);
        assert(result.id !== undefined && result.id.length > 0, 'result has id');
        assert(result.value !== undefined, 'result has value');
        assert(result.value.hello === 'world', 'value matches payload');

        client.close();
        await new Promise(r => setTimeout(r, 50));
    }

    console.log('2. Handler error');
    {
        router.on('Fail', (ctx) => {
            ctx.error('deliberate failure');
        });

        const client = new Client(ENDPOINT);
        await new Promise(r => setTimeout(r, 100));

        try {
            await client.perform('Fail', {}, TIMEOUT);
            assert(false, 'should have thrown');
        } catch (err) {
            assert(err.message.includes('deliberate failure'), 'error message propagated');
        }

        client.close();
        await new Promise(r => setTimeout(r, 50));
    }

    console.log('3. No handler (auto-error)');
    {
        const client = new Client(ENDPOINT);
        await new Promise(r => setTimeout(r, 100));

        try {
            await client.perform('NonExistent', { foo: 1 }, TIMEOUT);
            assert(false, 'should have thrown');
        } catch (err) {
            assert(err.message.includes('no handler'), 'auto-error for missing handler');
        }

        client.close();
        await new Promise(r => setTimeout(r, 50));
    }

    console.log('4. Multiple concurrent performs');
    {
        router.on('Add', (ctx) => {
            ctx.resume({ result: ctx.payload.a + ctx.payload.b });
        });

        const client = new Client(ENDPOINT);
        await new Promise(r => setTimeout(r, 100));

        const promises = [];
        for (let i = 0; i < 10; i++) {
            promises.push(client.perform('Add', { a: i, b: 10 }, TIMEOUT));
        }
        const results = await Promise.all(promises);

        assert(results.length === 10, 'all 10 results received');
        for (let i = 0; i < 10; i++) {
            assert(results[i].value.result === i + 10, `result[${i}] = ${i + 10}`);
        }

        client.close();
        await new Promise(r => setTimeout(r, 50));
    }

    console.log('5. Timeout');
    {
        router.on('Hang', (ctx) => {
            // intentionally not responding
        });

        const client = new Client(ENDPOINT);
        await new Promise(r => setTimeout(r, 100));

        try {
            await client.perform('Hang', {}, 300);
            assert(false, 'should have thrown');
        } catch (err) {
            assert(err.message.includes('timeout'), 'timeout error: ' + err.message);
        }

        client.close();
        await new Promise(r => setTimeout(r, 50));
    }

    router.close();
    await new Promise(r => setTimeout(r, 200));

    console.log(`\n=== Results: ${passed} passed, ${failed} failed ===`);
    process.exitCode = failed > 0 ? 1 : 0;
    try { require('..')._shutdown(); } catch {}
    process.exit(failed > 0 ? 1 : 0);
}

main().catch((err) => {
    console.error('Fatal error:', err);
    process.exit(1);
});
