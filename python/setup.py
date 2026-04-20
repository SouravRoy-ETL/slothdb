from setuptools import setup, find_packages

setup(
    name="slothdb",
    version="0.1.3",
    description="An embedded analytical database engine. Zero dependencies. GPU accelerated.",
    long_description=open("../README.md").read() if __import__("os").path.exists("../README.md") else "",
    long_description_content_type="text/markdown",
    author="Sourav Roy",
    author_email="souravroy7864@gmail.com",
    url="https://github.com/SouravRoy-ETL/slothdb",
    packages=find_packages(),
    python_requires=">=3.8",
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Intended Audience :: Science/Research",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: C++",
        "Topic :: Database",
        "Topic :: Database :: Database Engines/Servers",
        "Topic :: Scientific/Engineering",
    ],
    keywords="database olap analytics sql embedded parquet csv gpu",
    project_urls={
        "Bug Tracker": "https://github.com/SouravRoy-ETL/slothdb/issues",
        "Source": "https://github.com/SouravRoy-ETL/slothdb",
    },
)
