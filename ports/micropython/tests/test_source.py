# Trusted device module installed alongside the integration test.
def register(pipe, **kw):
    return {"unit": kw.get("unit", "C")}


def fetch(pipe, **kw):
    return {"tick": kw["tick"], "unit": pipe.parameters["unit"]}
