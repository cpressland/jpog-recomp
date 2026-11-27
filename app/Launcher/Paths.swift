// Where everything is: what the app carries, and what this Mac built.
import Foundation

enum Paths {
    static let title = "Jurassic Park: Operation Genesis"
    static let folderName = "Jurassic Park Operation Genesis"
    static let home: URL = {
        if let own = ProcessInfo.processInfo.environment["JPOG_HOME"], !own.isEmpty {
            return URL(fileURLWithPath: own)
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/\(folderName)")
    }()
    static let game = home.appendingPathComponent("game")
    static let library = home.appendingPathComponent("build/host/libjpog.dylib")

    static let logs = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/\(folderName)")
    static let gameLog = logs.appendingPathComponent("jpog.log")
    static let installLog = logs.appendingPathComponent("install.log")

    // What the app carries (app/build-app.sh).
    static let resources = Bundle.main.resourceURL!
    static let python = resources.appendingPathComponent("toolchain/python/bin/python3")
    static let ffmpeg = resources.appendingPathComponent("toolchain/ffmpeg")
    static let vorbis = resources.appendingPathComponent("toolchain/vorbis")
    static let installer = resources.appendingPathComponent("source/jpog/tools/install/install.py")
    static let pcrecomp = resources.appendingPathComponent("source/pcrecomp")
    static let frameworks = Bundle.main.privateFrameworksURL!

    static func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }
}
