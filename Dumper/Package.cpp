#include <algorithm>

#include "Package.h"
#include "Settings.h"
#include "Generator.h"

void Package::Process(std::vector<int32_t>& PackageMembers)
{
	for (int32_t Index : PackageMembers)
	{
		UEObject Object = ObjectArray::GetByIndex(Index);

		if (!Object || Object.HasAnyFlags(EObjectFlags::RF_ClassDefaultObject))
		{
			continue;
		}

		if (Object.GetOutermost() != PackageObject)
		{
			continue;
		}

		if (Object.IsA(EClassCastFlags::UEnum))
		{
			GenerateEnum(Object.Cast<UEEnum&>());
		}
		else if (Object.IsA(EClassCastFlags::UClass))
		{
			GenerateClass(Object.Cast<UEClass&>());
		}
		else if (Object.IsA(EClassCastFlags::UStruct) && !Object.IsA(EClassCastFlags::UFunction))
		{
			GenerateStruct(Object.Cast<UEStruct&>());
		}
	}
}

void Package::GenerateMembers(std::vector<UEProperty>& MemberVector, UEStruct& Super, Types::Struct& Struct)
{
	int PrevPropertyEnd = Super.GetSuper() ? Super.GetSuper().GetStructSize() : 0;
	int PrevBoolPropertyEnd = 0;
	int PrevBoolPropertyBit = 1;

	const int StructSize = Super.GetStructSize();
	std::string SuperName = Super.GetCppName();

	if (MemberVector.size() == 0)
	{
		if (Generator::PredefinedMembers.find(SuperName) != Generator::PredefinedMembers.end())
		{
			for (auto& Member : Generator::PredefinedMembers[SuperName])
			{
				if (Member.Offset > PrevPropertyEnd)
					Struct.AddMember(GenerateBytePadding(PrevPropertyEnd, Member.Offset - PrevPropertyEnd, "Fixing Size After Last (Predefined) Property  [ Dumper-7 ]"));

				Struct.AddMember(Types::Member(Member.Type, Member.Name, std::format("(0x{:02X}[0x{:02X}]) NOT AUTO-GENERATED PROPERTY", Member.Offset, Member.Size)));

				PrevPropertyEnd = Member.Offset + Member.Size;
			}

			if (StructSize > PrevPropertyEnd)
			{
				Struct.AddMember(GenerateBytePadding(PrevPropertyEnd, StructSize - PrevPropertyEnd, "Fixing Size Of Struct [ Dumper-7 ]"));
			}

			return;
		}
	}

	for (auto& Property : MemberVector)
	{
		std::string CppType = Property.GetCppType();
		std::string Name = Property.GetValidName();

		const int Offset = Property.GetOffset();
		const int Size = Property.GetSize();

		std::string Comment = std::format("0x{:X}(0x{:X})({})", Offset, Size, Property.StringifyFlags());

		if (Offset > PrevPropertyEnd)
		{
			Struct.AddMember(GenerateBytePadding(PrevPropertyEnd, Offset - PrevPropertyEnd, "Fixing Size After Last Property  [ Dumper-7 ]"));
		}

		if (Property.IsA(EClassCastFlags::UBoolProperty) &&  !Property.Cast<UEBoolProperty>().IsNativeBool())
		{
			Name += " : 1";

			const uint8 BitIndex = Property.Cast<UEBoolProperty>().GetBitIndex();

			Comment = std::format("Mask : 0x{:X} {}", Property.Cast<UEBoolProperty>().GetFieldMask(), Comment);

			if (PrevBoolPropertyEnd < Offset)
				PrevBoolPropertyBit = 1;

			if (PrevBoolPropertyBit < BitIndex)
			{
				Types::Member mem = GenerateBitPadding(Offset, BitIndex - PrevBoolPropertyBit, "Fixing Bit-Field Size  [ Dumper-7 ]");
				Struct.AddMember(mem);
			}

			PrevBoolPropertyBit = BitIndex + 1;
			PrevBoolPropertyEnd = Offset;
		}

		Types::Member Member(CppType, Name, Comment);

		PrevPropertyEnd = Offset + Size;

		Struct.AddMember(Member);
	}

	if (StructSize > PrevPropertyEnd)
		Struct.AddMember(GenerateBytePadding(PrevPropertyEnd, StructSize - PrevPropertyEnd, "Fixing Size Of Struct [ Dumper-7 ]"));
}

Types::Function Package::GenerateFunction(UEFunction& Function, UEStruct& Super)
{
	std::string ReturnType = "void";
	std::vector<Types::Parameter> Params;
	std::string FuncBody;

	std::vector<std::string> OutPtrParamNames;

	bool bHasRetType = false;
	
	for (UEProperty Param = Function.GetChild().Cast<UEProperty>(); Param; Param = Param.GetNext().Cast<UEProperty>())
	{
		bool bIsRef = false;
		bool bIsOut = false;
		bool bIsConst = false;

		std::string Type = Param.GetCppType();

		if (Param.HasPropertyFlags(EPropertyFlags::ConstParm))
		{
			Type = "const " + Type;
			bIsConst = true;
		}
		if (Param.HasPropertyFlags(EPropertyFlags::ReferenceParm))
		{
			Type += "&";
			bIsRef = true;
			bIsOut = true;
		}
		if (Param.HasPropertyFlags(EPropertyFlags::OutParm) && !bIsRef && !Param.HasPropertyFlags(EPropertyFlags::ReturnParm))
		{
			Type += "*";
			bIsOut = true;
			OutPtrParamNames.push_back(Param.GetValidName());
		}

		if (!bIsOut && !bIsRef && (Param.IsA(EClassCastFlags::UArrayProperty) || Param.IsA(EClassCastFlags::UStrProperty)))
		{
			Type += "&";
			
			if(!bIsConst)
				Type = "const " + Type;
		}

		if (Param.HasPropertyFlags(EPropertyFlags::ReturnParm))
		{
			ReturnType = Type;
			bHasRetType = true;
		}
		else
		{
			Params.push_back(Types::Parameter(Type, Param.GetValidName(), bIsOut && !bIsRef));
		}
	}
	
	Types::Function Func(ReturnType, Function.GetValidName(), Params, false);

	if (Settings::bShouldXorStrings)
	{
		FuncBody += std::format("\n\tstatic auto Func = GetFunction({0}(\"{}\"), {0}(\"{}\"));\n\n", Settings::XORString, Super.GetName(), Function.GetName());
	}
	else
	{
		FuncBody += std::format("\n\tstatic auto Func = GetFunction(\"{}\", \"{}\");\n\n", Super.GetName(), Function.GetName());
	}

	FuncBody += std::format("\t{}{} Parms;\n", (Settings::bUseNamespaceForParams ? Settings::ParamNamespaceName + std::string("::") : ""), Function.GetParamStructName());

	for (auto& Param : Func.GetParameters())
	{
		FuncBody += std::format("\tParms.{0} = {0};\n", Param.GetName());
	}

	if (Function.HasFlags(EFunctionFlags::Native))
		FuncBody += "\n\tauto Flags = Func->FunctionFlags;\n\tFunc->FunctionFlags |= 0x400;\n\n";

	FuncBody += "\n\tUObject::ProcessEvent(Func, &Parms);";

    if (Function.HasFlags(EFunctionFlags::Native))
        FuncBody += "\n\n\tFunc->FunctionFlags = Flags;\n\n";


	for (auto& Name : OutPtrParamNames)
	{
		FuncBody += std::format("\n\tif ({0} != nullptr)\n\t\t{0} = Parms.{0}\n", Name);
	}

	if (bHasRetType)
		FuncBody += "\n\treturn Parms.ReturnValue;\n";

	Func.AddBody(FuncBody);
	Func.SetParamStruct(GenerateStruct(Function, true));

	AllFunctions.push_back(Func);

	return Func;
}

Types::Struct Package::GenerateStruct(UEStruct& Struct, bool bIsFunction)
{
	std::string StructName = !bIsFunction ? Struct.GetCppName() : Struct.Cast<UEFunction>().GetParamStructName();

	Types::Struct RetStruct(StructName);

	int Size = Struct.GetStructSize();
	int SuperSize = 0;

	if (UEStruct Super = Struct.GetSuper())
	{
		RetStruct = Types::Struct(StructName, false, Super.GetCppName());
		SuperSize = Super.GetStructSize();
	}

	RetStruct.AddComment(std::format("0x{:X} (0x{:X} - 0x{:X})", Size - SuperSize, Size, SuperSize));
	RetStruct.AddComment(Struct.GetFullName());

	std::vector<UEProperty> Properties;

	static int NumProps = 0;
	static int NumFuncs = 0;

	for (UEField Child = Struct.GetChild(); Child; Child = Child.GetNext())
	{
		if (Child.IsA(EClassCastFlags::UProperty))
		{
			Properties.push_back(Child.Cast<UEProperty>());
		}
	}

	std::sort(Properties.begin(), Properties.end(), [](UEProperty Left, UEProperty Right) -> bool
		{
			if (Left.IsA(EClassCastFlags::UBoolProperty) && Right.IsA(EClassCastFlags::UBoolProperty))
			{
				if (Left.GetOffset() == Right.GetOffset())
				{
					return Left.Cast<UEBoolProperty>().GetFieldMask() < Right.Cast<UEBoolProperty>().GetFieldMask();
				}
			}

			return Left.GetOffset() < Right.GetOffset();
		});

	GenerateMembers(Properties, Struct, RetStruct);

	if (!bIsFunction)
		AllStructs.push_back(RetStruct);

	return RetStruct;
}

Types::Class Package::GenerateClass(UEClass& Class)
{
	std::string ClassName = Class.GetCppName();

	Types::Class RetClass(ClassName, Class.GetName());

	int Size = Class.GetStructSize();
	int SuperSize = 0;

	if (UEStruct Super = Class.GetSuper())
	{
		RetClass = Types::Class(ClassName, Class.GetName(), Super.GetCppName());
		SuperSize = Super.GetStructSize();
	}

	RetClass.AddComment(std::format("0x{:X} (0x{:X} - 0x{:X})", Size - SuperSize, Size, SuperSize));
	RetClass.AddComment(Class.GetFullName());

	std::vector<UEProperty> Properties;

	static int NumProps = 0;
	static int NumFuncs = 0;

	for (UEField Child = Class.GetChild(); Child; Child = Child.GetNext())
	{
		if (Child.IsA(EClassCastFlags::UProperty))
		{
			Properties.push_back(Child.Cast<UEProperty>());
		}
		else if (Child.IsA(EClassCastFlags::UFunction))
		{
			RetClass.AddFunction(GenerateFunction(Child.Cast<UEFunction&>(), Class));
		}
	}

	std::sort(Properties.begin(), Properties.end(), [](UEProperty Left, UEProperty Right) -> bool
		{
			if (Left.IsA(EClassCastFlags::UBoolProperty) && Right.IsA(EClassCastFlags::UBoolProperty))
			{
				if (Left.GetOffset() == Right.GetOffset())
				{
					return Left.Cast<UEBoolProperty>().GetFieldMask() < Right.Cast<UEBoolProperty>().GetFieldMask();
				}
			}

			return Left.GetOffset() < Right.GetOffset();
		});

	GenerateMembers(Properties, Class, RetClass);

	AllClasses.push_back(RetClass);

	return RetClass;
}

Types::Enum Package::GenerateEnum(UEEnum& Enum)
{
	Types::Enum Enm(Enum.GetEnumTypeAsStr(), "uint8");

	auto NameValue = Enum.GetNameValuePairs();

	for (int i = 0; i < NameValue.Num(); i++)
	{
		std::string TooFullOfAName = NameValue[i].First.ToString();

		Enm.AddMember(TooFullOfAName.substr(TooFullOfAName.find_last_of(":") + 1), NameValue[i].Second);
	}

	AllEnums.push_back(Enm);

	return Enm;
}

Types::Member Package::GenerateBytePadding(int32 Offset, int32 PadSize, std::string&& Reason)
{
	static uint32 PadNum = 0;

	return Types::Member("uint8", std::format("Pad_{:X}[0x{:X}]", PadNum++, PadSize), Reason);
}

Types::Member Package::GenerateBitPadding(int32 Offset, int32 PadSize, std::string&& Reason)
{
	return Types::Member("uint8", std::format(": {:X}", PadSize), Reason);
}