import os, subprocess
W = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'work')
p = os.path.join(W, 'engine/src/rhi/webgpu_device.cpp'); src = open(p).read()
for what, old, new in [
  ('additive colour blend', 'blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};', 'blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One};'),
  ('pre-fix alpha component (dst alpha blended)', 'blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};', 'blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};')]:
    assert src.count(old) == 1
    open(p, 'w').write(src.replace(old, new))
    try:
        subprocess.run('timeout 900 cmake --build build/native', cwd=W, shell=True, check=True, capture_output=True)
        t = subprocess.run('./build/native/lomn_gpu_area_test', cwd=W, shell=True, capture_output=True, text=True)
        print(what, '->', 'CAUGHT' if t.returncode != 0 else 'MISSED', [l for l in t.stdout.splitlines() if 'blend' in l])
    finally:
        open(p, 'w').write(src)
subprocess.run('timeout 900 cmake --build build/native', cwd=W, shell=True, check=True, capture_output=True)
print('restored; gpu test rc', subprocess.run('./build/native/lomn_gpu_area_test', cwd=W, shell=True, capture_output=True).returncode)
