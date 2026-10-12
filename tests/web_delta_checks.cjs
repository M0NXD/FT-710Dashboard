const {execFileSync}=require('node:child_process');
const assert=require('node:assert/strict');
const {mergeState}=require('../web/control-core.js');
const cases=execFileSync(process.argv[2],{encoding:'utf8'}).trim().split(/\r?\n/).map(line=>JSON.parse(line));
for(const {before,after,patch}of cases){const result=patch.type==='state'?patch:mergeState(before,patch);assert.deepEqual(result,after);}
console.log(`Web delta checks passed: ${cases.length} server/client round trips (readings, log append/trim/clear, catalog and generation changes).`);
