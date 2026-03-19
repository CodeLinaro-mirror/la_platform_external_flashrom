//
// Copyright 2019, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Alternatively, this software may be distributed under the terms of the
// GNU General Public License ("GPL") version 2 as published by the Free
// Software Foundation.
//

use std::convert::TryInto;
use std::io::prelude::*;
use std::process::Command;



#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub struct LayoutSizes {
    half_sz: i64,
    quad_sz: i64,
    eighth_sz: i64,
    rom_top: i64,
    bottom_half_top: i64,
    bottom_quad_top: i64,
    bottom_eighth_top: i64,
    top_quad_bottom: i64,
    top_eighth_bottom: i64,
}

pub fn get_layout_sizes(rom_sz: i64) -> Result<LayoutSizes, String> {
    if rom_sz <= 0 {
        return Err("invalid rom size provided".into());
    }
    if rom_sz & (rom_sz - 1) != 0 {
        return Err("invalid rom size, not a power of 2".into());
    }
    Ok(LayoutSizes {
        half_sz: rom_sz / 2,
        quad_sz: rom_sz / 4,
        eighth_sz: rom_sz / 8,
        rom_top: rom_sz - 1,
        bottom_half_top: (rom_sz / 2) - 1,
        bottom_quad_top: (rom_sz / 4) - 1,
        bottom_eighth_top: (rom_sz / 8) - 1,
        top_quad_bottom: (rom_sz / 4) * 3,
        top_eighth_bottom: (rom_sz / 8) * 7,
    })
}



pub fn construct_layout_file<F: Write>(mut target: F, ls: &LayoutSizes) -> std::io::Result<()> {
    writeln!(target, "000000:{:x} BOTTOM_EIGHTH", ls.bottom_eighth_top)?;
    writeln!(target, "000000:{:x} BOTTOM_QUAD", ls.bottom_quad_top)?;
    writeln!(target, "000000:{:x} BOTTOM_HALF", ls.bottom_half_top)?;
    writeln!(target, "{:x}:{:x} TOP_HALF", ls.half_sz, ls.rom_top)?;
    writeln!(target, "{:x}:{:x} TOP_QUAD", ls.top_quad_bottom, ls.rom_top)?;
    writeln!(
        target,
        "{:x}:{:x} TOP_EIGHTH",
        ls.top_eighth_bottom, ls.rom_top
    )
}

pub fn toggle_hw_wp(dis: bool) -> Result<(), String> {
    // The easist way to toggle the hardware write-protect is
    // to {dis}connect the battery (and/or {open,close} the WP screw).
    let s = if dis { "dis" } else { "" };
    let screw_state = if dis { "open" } else { "close" };
    // Print a failure message, but not on the first try.
    let mut fail_msg = None;
    while dis == get_hardware_wp()? {
        if let Some(msg) = fail_msg {
            eprintln!("{msg}");
        }
        fail_msg = Some(format!("Hardware write protect is still {}!", !dis));
        // The following message is read by the tast test. Do not modify.
        info!("Prompt for hardware WP {}able", s);
        eprintln!(
            " > {}connect the battery (and/or {} the WP screw)",
            s, screw_state
        );
        pause();
    }
    Ok(())
}

pub fn ac_power_warning() {
    info!("*****************************");
    info!("AC power *must be* connected!");
    info!("*****************************");
    pause();
}

fn pause() {
    // The following message is read by the tast test. Do not modify.
    println!("Press enter to continue...");
    // Rust stdout is always LineBuffered at time of writing.
    // But this is not guaranteed, so flush anyway.
    std::io::stdout().flush().unwrap();
    // This reads one line, there is no guarantee the line came
    // after the above prompt. But it is good enough.
    if std::io::stdin().read_line(&mut String::new()).unwrap() == 0 {
        panic!("stdin closed");
    }
}

pub fn get_hardware_wp() -> std::result::Result<bool, String> {
    let wp_s_val = collect_crosssystem(&["wpsw_cur"])?.parse::<u32>();
    match wp_s_val {
        Ok(v) => {
            if v == 1 {
                Ok(true)
            } else if v == 0 {
                Ok(false)
            } else {
                Err("Unknown write protect value".into())
            }
        }
        Err(_) => Err("Cannot parse write protect value".into()),
    }
}

pub fn collect_crosssystem(args: &[&str]) -> Result<String, String> {
    let cmd = match Command::new("crossystem").args(args).output() {
        Ok(x) => x,
        Err(e) => return Err(format!("Failed to run crossystem: {}", e)),
    };

    if !cmd.status.success() {
        return Err(translate_command_error(&cmd).to_string());
    };

    Ok(String::from_utf8_lossy(&cmd.stdout).into_owned())
}

pub fn translate_command_error(output: &std::process::Output) -> std::io::Error {
    use std::io::{Error, ErrorKind};
    // There is two cases on failure;
    //  i. ) A bad exit code,
    //  ii.) A SIG killed us.
    match output.status.code() {
        Some(code) => {
            let e = format!(
                "{}\nExited with error code: {}",
                String::from_utf8_lossy(&output.stderr),
                code
            );
            Error::new(ErrorKind::Other, e)
        }
        None => Error::new(
            ErrorKind::Other,
            "Process terminated by a signal".to_string(),
        ),
    }
}

/// A simple manual FMAP parser to find the offset and length of a region.
/// This prevents relying on external tools for testing non-standard ranges.
///
/// FMAP Structure Reference:
/// struct fmap {
///     uint8_t  signature[8]; // "__FMAP__"
///     uint8_t  ver_major;
///     uint8_t  ver_minor;
///     uint64_t base;
///     uint32_t size;
///     uint8_t  name[32];
///     uint16_t nareas;
/// } __attribute__((packed));
///
/// struct fmap_area {
///     uint32_t offset;
///     uint32_t size;
///     uint8_t  name[32];
///     uint16_t flags;
/// } __attribute__((packed));
pub fn find_fmap_region(data: &[u8], region_name: &str) -> std::result::Result<(i64, i64), String> {
    const SIGNATURE: &[u8] = b"__FMAP__";

    // 1. Validate FMAP Header
    if data.len() < 56 || &data[0..8] != SIGNATURE {
        return Err("Data does not start with a valid FMAP signature".into());
    }

    let ver_major = data[8];
    let ver_minor = data[9];
    let nareas = u16::from_le_bytes([data[54], data[55]]);

    debug!(
        "FMAP version {}.{} found, containing {} areas.",
        ver_major, ver_minor, nareas
    );

    if nareas == 0 {
        return Err("FMAP contains zero areas".into());
    }

    // 2. Iterate over Area Descriptors
    let mut current_offset = 56;
    for _ in 0..nareas {
        if current_offset + 42 > data.len() {
            break;
        }

        let area_offset = u32::from_le_bytes(
            data[current_offset..current_offset + 4]
                .try_into()
                .map_err(|_| "Failed to parse area offset")?,
        ) as i64;
        let area_size = u32::from_le_bytes(
            data[current_offset + 4..current_offset + 8]
                .try_into()
                .map_err(|_| "Failed to parse area size")?,
        ) as i64;

        // Name is 32 bytes, null-padded
        let name_at = current_offset + 8;
        let name_bytes = &data[name_at..name_at + 32];

        let null_pos = name_bytes.iter().position(|&c| c == 0).unwrap_or(32);
        let area_name = String::from_utf8_lossy(&name_bytes[0..null_pos]);

        if area_name == region_name {
            info!(
                "Found {} in FMAP: offset={:#x}, size={:#x}",
                region_name, area_offset, area_size
            );
            return Ok((area_offset, area_size));
        }

        current_offset += 42;
    }

    Err(format!("Region {} not found in FMAP", region_name))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn construct_layout_file() {
        use super::{construct_layout_file, get_layout_sizes};

        let mut buf = Vec::new();
        construct_layout_file(
            &mut buf,
            &get_layout_sizes(0x10000).expect("64k is a valid chip size"),
        )
        .expect("no I/O errors expected");

        assert_eq!(
            &buf[..],
            &b"000000:1fff BOTTOM_EIGHTH\n\
               000000:3fff BOTTOM_QUAD\n\
               000000:7fff BOTTOM_HALF\n\
               8000:ffff TOP_HALF\n\
               c000:ffff TOP_QUAD\n\
               e000:ffff TOP_EIGHTH\n"[..]
        );
    }

    #[test]
    fn get_layout_sizes() {
        use super::get_layout_sizes;

        assert_eq!(
            get_layout_sizes(-128).err(),
            Some("invalid rom size provided".into())
        );

        assert_eq!(
            get_layout_sizes(3 << 20).err(),
            Some("invalid rom size, not a power of 2".into())
        );

        assert_eq!(
            get_layout_sizes(64 << 10).unwrap(),
            LayoutSizes {
                half_sz: 0x8000,
                quad_sz: 0x4000,
                eighth_sz: 0x2000,
                rom_top: 0xFFFF,
                bottom_half_top: 0x7FFF,
                bottom_quad_top: 0x3FFF,
                bottom_eighth_top: 0x1FFF,
                top_quad_bottom: 0xC000,
                top_eighth_bottom: 0xE000,
            }
        );
    }

    #[test]
    fn test_find_fmap_region() {
        let mut data = vec![0u8; 1000];
        // Write signature at the beginning
        data[0..8].copy_from_slice(b"__FMAP__");
        // Write ver_major (1) and ver_minor (1)
        data[8] = 1;
        data[9] = 1;
        // Write nareas (2)
        data[54..56].copy_from_slice(&2u16.to_le_bytes());

        // Area 1: WP_RO
        let mut area1 = vec![0u8; 42];
        area1[0..4].copy_from_slice(&0x1000u32.to_le_bytes()); // Offset
        area1[4..8].copy_from_slice(&0x2000u32.to_le_bytes()); // Size
        area1[8..13].copy_from_slice(b"WP_RO"); // Name
        data[56..98].copy_from_slice(&area1);

        // Area 2: RW_MISC
        let mut area2 = vec![0u8; 42];
        area2[0..4].copy_from_slice(&0x4000u32.to_le_bytes()); // Offset
        area2[4..8].copy_from_slice(&0x5000u32.to_le_bytes()); // Size
        area2[8..15].copy_from_slice(b"RW_MISC"); // Name
        data[98..140].copy_from_slice(&area2);

        let (offset, size) = super::find_fmap_region(&data, "WP_RO").unwrap();
        assert_eq!(offset, 0x1000);
        assert_eq!(size, 0x2000);

        let (offset, size) = super::find_fmap_region(&data, "RW_MISC").unwrap();
        assert_eq!(offset, 0x4000);
        assert_eq!(size, 0x5000);

        // Test with invalid UTF-8 name (0xFF) in FMAP
        let mut area3 = vec![0u8; 42];
        area3[0..4].copy_from_slice(&0x6000u32.to_le_bytes()); // Offset
        area3[4..8].copy_from_slice(&0x7000u32.to_le_bytes()); // Size
        area3[8..10].copy_from_slice(&[0xff, 0xff]); // Invalid UTF-8
        data[140..182].copy_from_slice(&area3);
        // Increment nareas to 3
        data[54..56].copy_from_slice(&3u16.to_le_bytes());

        // Should still find standard regions fine
        assert!(super::find_fmap_region(&data, "WP_RO").is_ok());

        // Test not found
        assert!(super::find_fmap_region(&data, "NOT_FOUND").is_err());

        // Test signature not at beginning
        let mut data_not_at_start = vec![0u8; 1000];
        data_not_at_start[10..18].copy_from_slice(b"__FMAP__");
        assert!(super::find_fmap_region(&data_not_at_start, "WP_RO").is_err());
    }
}
