# Adding passes to LLPC

The process of creating and adding a pass to LLPC is very similar to LLVM's process.
You first need to create your pass. See https://llvm.org/docs/WritingAnLLVMNewPMPass.html for the LLVM documentation explaining how to create a pass.
As an example, here is the minimal code required to create a module pass:

```
class MyPass : public llvm::PassInfoMixin<MyPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager) {
    // do something with the module...
    return llvm::PreservedAnalyses::none();
  }
  static llvm::StringRef name() { return "Full name of the pass"; }
};
```

Please, implement a static `name()` function like in this example for each pass added to LLPC so the passes can easily be identified in logs.
Once the pass is created, it needs to be registered to LLPC.

## Register a front-end pass

To register a front-end pass, you need to add an entry to the LLPC front-end pass registry (`llpc/lower/PassRegistry.inc`). For example:

`LLPC_PASS("short-name", MyPass)`

* `short-name` is a unique dash-separated short name used to identify the pass. This short name is for example used for the `--print-after` option to print LLVM IR after a given pass short name (e.g. `--print-after=short-name`).
* `MyPass` is the name of the pass class.

## Register a middle-end pass

The process to register a middle-end pass is the same. The middle-end pass registry file is `lgc/patch/PassRegistry.inc`.
