# Shared GPU/visual launch flags for play.ps1 and fix_loop.ps1. Dot-source, then call Get-LookArgs.
#   Sharp     2x, FXAA extreme, CAS 0.5, no DOF/bloom         (light, the original "sharp" setup)
#   Clean     3x, FXAA, CAS 0.3, dither, no DOF/bloom
#   Ultra     4x, FXAA, CAS 0.25, dither, no DOF/bloom        (heaviest; watch VRAM/FPS)
#   Pure      2x, NO blur buffer at all (sharp scene in the bloom slot = old Crisp), Exposure 1.55, Gamma 1.05 (gamma lift reveals banding; brightness via exposure)  [default]
#   Crisp     2x, light FXAA, CAS 0.7, dither, real bloom at 15%, Exposure 2.2, Gamma 1.1
#   Composite PS 32B3EC0C91CA5BCC: out = gamma(sat((bloom320 * c1 + scene) * c2)), game c1 = 0.4, c2 = 1.
#   Bloom scales c1, Exposure scales c2 (overall brightness), Gamma divides the output gamma c254.z (shadow lift, >1 brighter). Delta shifts the scene in whole stops.
#   Cinematic 3x, no FXAA, CAS 0.7, dither, bloom at 50%, Exposure 1.6
$Looks = @{
    Sharp     = @{ Scale = 2; AA = 'fxaa_extreme'; Sharpness = 0.5;  Dither = $false; Dof = $false; Delta = -3 }
    Clean     = @{ Scale = 3; AA = 'fxaa';         Sharpness = 0.3;  Dither = $true;  Dof = $false; Delta = -3 }
    Ultra     = @{ Scale = 4; AA = 'fxaa';         Sharpness = 0.25; Dither = $true;  Dof = $false; Delta = -3 }
    Pure      = @{ Scale = 2; AA = 'fxaa';         Sharpness = 0.7;  Dither = $true;  Dof = $false; Delta = -4; Bloom = 1.0;  Exposure = 1.55; Gamma = 1.05 }
    Crisp     = @{ Scale = 2; AA = 'fxaa';         Sharpness = 0.7;  Dither = $true;  Dof = $true;  Delta = -4; Bloom = 0.15; Exposure = 2.2; Gamma = 1.1 }
    Cinematic = @{ Scale = 3; AA = 'none';         Sharpness = 0.7;  Dither = $true;  Dof = $true;  Delta = -4; Bloom = 0.5;  Exposure = 1.6; Gamma = 1.25 }
    # BioShock 2 (bioshock2-recomp): same blur fix, one stop darker, the BS1-only composite pass skipped
    # (SkipComposite). Picked from a bracket on 2026-11-04: delta -3 / exposure 1.0 (the warm one).
    BS2       = @{ Scale = 2; AA = 'fxaa';         Sharpness = 0.7;  Dither = $true;  Dof = $false; Delta = -3; Bloom = 1.0;  Exposure = 1.0; Gamma = 1.05; SkipComposite = $true }
}

function Get-LookArgs([hashtable]$L, [int]$Delta = $L.Delta, [int]$Aniso = 5, [string]$Present = 'cas',
                      [bool]$DirectResolve = $true, [bool]$SkipComposite = $false, [int]$GuestHz = 120) {
    @('--gpu_plugin', 'xenos',
      # Blur fix (see docs/TIMELINE.md)
      '--resolve_1011_exp_bias=0', "--fetch_1011_exp_adjust_delta=$Delta", '--skip_hdr_1011_resolve_bias=-3',
      $(if ($L.Dof) { '--no-bioshock_sharp_composite' } else { '--bioshock_sharp_composite' }),
      # Image quality
      "--resolution_scale=$($L.Scale)", "--anisotropic_override=$Aniso",
      "--present_effect=$Present", "--present_cas_additional_sharpness=$($L.Sharpness)",
      "--bioshock_bloom_scale=$(if ($null -ne $L.Bloom) { $L.Bloom } else { 1.0 })",
      "--bioshock_gamma=$(if ($null -ne $L.Gamma) { $L.Gamma } else { 1.0 })",
      "--bioshock_exposure_scale=$(if ($null -ne $L.Exposure) { $L.Exposure } else { 1.0 })",
      "--swap_post_effect=$($L.AA)", "--present_dither=$($L.Dither.ToString().ToLower())",
      # Stutter / texture churn: defaults (384/768/24 MB) are sized for weak GPUs
      '--texture_cache_memory_limit_soft=2048', '--texture_cache_memory_limit_hard=4096',
      '--texture_cache_memory_limit_render_to_texture=128', '--hitch_log_ms=50', '--draw_resolution_scaled_texture_offsets=false', "--video_mode_refresh_rate=$GuestHz",
      $(if ($SkipComposite) { '--skip_pixel_shaders=32B3EC0C91CA5BCC' } else { '--skip_pixel_shaders=0' }),
      "--direct_host_resolve=$($DirectResolve.ToString().ToLower())")
}
