// SPDX-License-Identifier: LGPL-2.1-or-later
import Foundation

/// Reads the import tables of a Windows executable to find out which DLLs it links against.
public struct PEInfo: Equatable, Sendable {
    public var is64Bit: Bool
    /// Lower-cased DLL names of the import and delay-import tables.
    public var imports: Set<String>
    /// Imports of d3d12.dll by ordinal rather than by name (the layer exports both).
    public var importsD3D12ByOrdinal: Bool

    public var importsD3D12: Bool { imports.contains("d3d12.dll") }
}

public enum PEScanner {
    /// Parses the headers and import tables. Returns nil if the file is not a PE image.
    public static func scan(url: URL) -> PEInfo? {
        guard let data = try? Data(contentsOf: url, options: .alwaysMapped) else { return nil }
        return scan(data: data)
    }

    public static func scan(data: Data) -> PEInfo? {
        let r = Reader(data)
        guard data.count > 0x40, r.u16(0) == 0x5A4D else { return nil }
        let peOffset = Int(r.u32(0x3C))
        guard peOffset > 0, peOffset + 24 < data.count, r.u32(peOffset) == 0x0000_4550 else { return nil }
        let numSections = Int(r.u16(peOffset + 6))
        let optSize = Int(r.u16(peOffset + 20))
        let opt = peOffset + 24
        guard opt + 2 <= data.count else { return nil }
        let magic = r.u16(opt)
        let is64: Bool
        switch magic {
        case 0x20B: is64 = true
        case 0x10B: is64 = false
        default: return nil
        }
        let dirBase = opt + (is64 ? 112 : 96)
        let numDirs = Int(r.u32(dirBase - 4))
        func dir(_ i: Int) -> (rva: Int, size: Int)? {
            guard i < numDirs, dirBase + i * 8 + 8 <= data.count, dirBase + i * 8 + 8 <= opt + optSize else { return nil }
            let rva = Int(r.u32(dirBase + i * 8))
            return rva == 0 ? nil : (rva, Int(r.u32(dirBase + i * 8 + 4)))
        }
        // Sections for RVA -> file offset.
        var sections: [(va: Int, size: Int, raw: Int)] = []
        let secTable = opt + optSize
        for i in 0..<numSections {
            let o = secTable + i * 40
            guard o + 40 <= data.count else { break }
            let vsize = Int(r.u32(o + 8)), va = Int(r.u32(o + 12)), rawSize = Int(r.u32(o + 16)), raw = Int(r.u32(o + 20))
            sections.append((va, max(vsize, rawSize), raw))
        }
        func offset(_ rva: Int) -> Int? {
            for s in sections where rva >= s.va && rva < s.va + s.size { return rva - s.va + s.raw }
            return nil
        }
        func cString(_ rva: Int) -> String? {
            guard var o = offset(rva) else { return nil }
            var bytes: [UInt8] = []
            while o < data.count, data[o] != 0, bytes.count < 260 { bytes.append(data[o]); o += 1 }
            return String(decoding: bytes, as: UTF8.self).lowercased()
        }

        var imports = Set<String>()
        var ordinal = false
        let thunkSize = is64 ? 8 : 4
        let ordinalFlag: UInt64 = is64 ? 0x8000_0000_0000_0000 : 0x8000_0000

        // IMAGE_DIRECTORY_ENTRY_IMPORT = 1: descriptors of 20 bytes.
        if let d = dir(1), var o = offset(d.rva) {
            while o + 20 <= data.count {
                let nameRVA = Int(r.u32(o + 12))
                if nameRVA == 0 { break }
                if let name = cString(nameRVA) {
                    imports.insert(name)
                    if name == "d3d12.dll" {
                        let lookup = Int(r.u32(o)) != 0 ? Int(r.u32(o)) : Int(r.u32(o + 16))
                        if var t = offset(lookup) {
                            while t + thunkSize <= data.count {
                                let v = is64 ? r.u64(t) : UInt64(r.u32(t))
                                if v == 0 { break }
                                if v & ordinalFlag != 0 { ordinal = true }
                                t += thunkSize
                            }
                        }
                    }
                }
                o += 20
            }
        }
        // IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT = 13: descriptors of 32 bytes (name RVA at +4).
        if let d = dir(13), var o = offset(d.rva) {
            while o + 32 <= data.count {
                let nameRVA = Int(r.u32(o + 4))
                if nameRVA == 0 { break }
                if let name = cString(nameRVA) { imports.insert(name) }
                o += 32
            }
        }
        return PEInfo(is64Bit: is64, imports: imports, importsD3D12ByOrdinal: ordinal)
    }

    /// True when the image links d3d12.dll, or mentions it as a string (a game that loads it with
    /// LoadLibrary: launchers that pick D3D11 or D3D12 at run time). The string search is a hint, not proof.
    public static func usesD3D12(url: URL) -> Bool {
        guard let data = try? Data(contentsOf: url, options: .alwaysMapped), let info = scan(data: data) else { return false }
        if info.importsD3D12 { return true }
        return containsDynamicReference(data)
    }

    static func containsDynamicReference(_ data: Data) -> Bool {
        let needles: [[UInt8]] = [
            Array("D3D12CreateDevice".utf8),
            Array("d3d12.dll".utf8), Array("D3D12.dll".utf8), Array("D3D12.DLL".utf8),
            "d3d12.dll".utf16.flatMap { [UInt8($0), 0] },
        ]
        return data.withUnsafeBytes { raw -> Bool in
            guard let base = raw.baseAddress else { return false }
            for n in needles where n.count > 0 {
                if memmem(base, raw.count, n, n.count) != nil { return true }
            }
            return false
        }
    }
}

/// Little-endian reads that never trap on short data (out of range reads return 0).
struct Reader {
    let data: Data
    init(_ d: Data) { data = d }
    func u16(_ o: Int) -> UInt16 {
        guard o >= 0, o + 2 <= data.count else { return 0 }
        let b = data.startIndex + o
        return UInt16(data[b]) | UInt16(data[b + 1]) << 8
    }
    func u32(_ o: Int) -> UInt32 {
        guard o >= 0, o + 4 <= data.count else { return 0 }
        return UInt32(u16(o)) | UInt32(u16(o + 2)) << 16
    }
    func u64(_ o: Int) -> UInt64 { UInt64(u32(o)) | UInt64(u32(o + 4)) << 32 }
}
