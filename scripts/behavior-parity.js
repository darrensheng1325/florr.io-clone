'use strict';

const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = path.resolve(__dirname, '..');
const build = path.join(root, 'cpp', 'build');

function run(command, args, options = {}) {
    const result = spawnSync(command, args, {
        cwd: root,
        encoding: 'utf8',
        maxBuffer: 128 * 1024 * 1024,
        ...options,
    });
    if (result.status !== 0) {
        process.stderr.write(result.stdout || '');
        process.stderr.write(result.stderr || '');
        process.exit(result.status === null ? 1 : result.status);
    }
    return result.stdout;
}

// Reconfigure even when a cache exists so a newly-added probe target is
// available in an older developer build directory.
run('cmake', ['-S', 'cpp', '-B', 'cpp/build']);
run('cmake', ['--build', 'cpp/build', '--target', 'behavior_oracle', '-j8']);
const tsOutDir = fs.mkdtempSync(path.join(os.tmpdir(), 'florr-behavior-'));
run(process.execPath, [
    path.join(root, 'node_modules', 'typescript', 'bin', 'tsc'),
    '-p', 'tsconfig.behavior.json', '--outDir', tsOutDir,
]);
const tsOutput = run(process.execPath, [path.join(tsOutDir, 'server', 'behaviorOracle.js')]);
fs.rmSync(tsOutDir, { recursive: true, force: true });
const cppOutput = run(path.join(build, 'behavior_oracle'), [
    path.join(root, 'src', 'mobs.json'),
    path.join(root, 'src', 'petals.json'),
    path.join(root, 'cpp', 'data', 'mob_xp.json'),
]);

// The movement scenario is TypeScript-only now.
//
// Both oracles used to step the same walls, because both read the tile grid
// out of src/map_bundle.ts. The C++ game's maps are authored in Tiled and the
// frozen TypeScript build cannot read a .tmj, so there is no longer a map the
// two sides can agree on -- and stepping two DIFFERENT maps would report a
// parity failure that says nothing about the movement code. The C++ movement
// step is covered against real terrain by cpp/tests/movement_tests.cpp.
//
// Everything else -- rarity maths, mob and petal stats, modifiers, combat --
// is still compared observation for observation.
const kTypeScriptOnly = /^scenario\/movement\//;

function parse(label, output) {
    const metrics = new Map();
    for (const line of output.trim().split('\n')) {
        if (!line) continue;
        const row = JSON.parse(line);
        if (!Array.isArray(row) || row.length !== 2 || typeof row[0] !== 'string') {
            throw new Error(`${label} emitted malformed row: ${line}`);
        }
        if (metrics.has(row[0])) throw new Error(`${label} emitted duplicate metric: ${row[0]}`);
        metrics.set(row[0], row[1]);
    }
    return metrics;
}

const ts = parse('TypeScript', tsOutput);
const cpp = parse('C++', cppOutput);
const keys = new Set([...ts.keys(), ...cpp.keys()]);
const differences = [];

function equal(left, right) {
    if (typeof left !== typeof right) return false;
    if (typeof left !== 'number') return left === right;
    if (!Number.isFinite(left) || !Number.isFinite(right)) return Object.is(left, right);
    const scale = Math.max(1, Math.abs(left), Math.abs(right));
    return Math.abs(left - right) <= 1e-9 * scale;
}

for (const key of [...keys].sort()) {
    if (kTypeScriptOnly.test(key) && !cpp.has(key)) continue;
    if (!ts.has(key)) differences.push(`${key}: missing in TypeScript, C++=${JSON.stringify(cpp.get(key))}`);
    else if (!cpp.has(key)) differences.push(`${key}: TypeScript=${JSON.stringify(ts.get(key))}, missing in C++`);
    else if (!equal(ts.get(key), cpp.get(key))) {
        differences.push(`${key}: TypeScript=${JSON.stringify(ts.get(key))}, C++=${JSON.stringify(cpp.get(key))}`);
    }
}

if (differences.length) {
    console.error(`Behavior parity FAILED: ${differences.length} of ${keys.size} observations differ.`);
    for (const difference of differences.slice(0, 200)) console.error(`  ${difference}`);
    if (differences.length > 200) console.error(`  ... ${differences.length - 200} more`);
    process.exit(1);
}

console.log(`Behavior parity passed: ${keys.size} TypeScript/C++ observations are identical.`);
