class Slothdb < Formula
  desc "An embedded analytical database engine. Zero dependencies. GPU accelerated."
  homepage "https://github.com/SouravRoy-ETL/slothdb"
  url "https://github.com/SouravRoy-ETL/slothdb/archive/refs/tags/v0.1.2.tar.gz"
  sha256 "" # Fill after release
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-B", "build",
           "-DSLOTHDB_BUILD_SHELL=ON",
           "-DSLOTHDB_BUILD_TESTS=OFF",
           *std_cmake_args
    system "cmake", "--build", "build", "--config", "Release"
    bin.install "build/src/slothdb"
  end

  test do
    output = shell_output("echo 'SELECT 42 AS answer;' | #{bin}/slothdb")
    assert_match "42", output
  end
end
