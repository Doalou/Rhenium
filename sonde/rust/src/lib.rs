//! Test minimal de la toolchain Rust UWP (thumbv7a-uwp-windows-msvc sur Lumia) pour Rhénium :
//! la std (Vec, tri, format!, parse) et un thread, appelés depuis le C++ de la sonde.

use std::thread;

/// Renvoie 5050 si tout fonctionne : la somme de 1 à 100, calculée sur un thread après un tri,
/// puis passée par une chaîne formatée et relue. Toute autre valeur indique l'étape fautive.
#[no_mangle]
pub extern "C" fn rhenium_rust_probe() -> u32 {
    let worker = thread::spawn(|| {
        let mut values: Vec<u32> = (1..=100).rev().collect();
        values.sort_unstable();
        values.iter().sum::<u32>()
    });
    let sum = match worker.join() {
        Ok(sum) => sum,
        Err(_) => return 1,
    };
    format!("{sum}").parse::<u32>().unwrap_or(2)
}
