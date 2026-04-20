with open("reference_codebases/projectm/vendor/hlslparser/src/GLSLGenerator.cpp", "r") as f:
    code = f.read()

old_str = """    m_writer.EndLine();
    printf("GLSL_GEN_BEGIN\\n%s\\nGLSL_GEN_END\\n", m_writer.GetResult());
    return !m_error;"""

new_str = """    m_writer.EndLine();

    return !m_error;"""

if old_str in code:
    code = code.replace(old_str, new_str)
    with open("reference_codebases/projectm/vendor/hlslparser/src/GLSLGenerator.cpp", "w") as f:
        f.write(code)
    print("Cleaned up successfully")
else:
    print("Could not find debug print")
