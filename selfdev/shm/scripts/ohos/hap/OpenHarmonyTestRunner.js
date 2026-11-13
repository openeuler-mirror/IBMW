// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Test runner of the shm_ext test HAP, started by
// `aa test ... -s repeat <n> -s steptimeout <seconds per EXT run>`.
// The NAPI module runs the tests as native child processes (see
// test_peer/shm_ext_hap.cc); this polls it once a second and prints its summary.
import AbilityDelegatorRegistry from '@ohos:app.ability.abilityDelegatorRegistry';
import shm from '@app:@BUNDLE@/entry/ibmw_shm_ext_hap';

export default class OpenHarmonyTestRunner {
  onPrepare() {}

  onRun() {
    const delegator = AbilityDelegatorRegistry.getAbilityDelegator();
    const params = AbilityDelegatorRegistry.getArguments().parameters;
    const arg = (name, fallback) => parseInt(params['-s ' + name] ?? params[name] ?? fallback);
    try {
      shm.start(delegator.getAppContext().cacheDir, arg('repeat', '1'), arg('steptimeout', '1800'));
    } catch (e) {
      delegator.printSync('HAP_RESULT result=FAIL start: ' + e);
      delegator.finishTest('IBMW_HAP done', 1, () => {});
      return;
    }
    const timer = setInterval(() => {
      const summary = shm.poll();
      if (summary.length === 0) {
        return;
      }
      clearInterval(timer);
      delegator.printSync(summary);
      delegator.finishTest('IBMW_HAP done', summary.includes('HAP_RESULT result=PASS') ? 0 : 1, () => {});
    }, 1000);
  }
}
